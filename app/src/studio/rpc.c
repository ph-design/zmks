/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "msg_framing.h"

#include <pb_encode.h>
#include <pb_decode.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/debug/thread_analyzer.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_ZMK_STUDIO_TRANSPORT_UART)
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#endif
#include <zmk/studio/core.h>
#include <zmk/studio/rpc.h>

ZMK_EVENT_IMPL(zmk_studio_rpc_notification);

static struct zmk_rpc_subsystem *find_subsystem_for_choice(uint8_t choice) {
    STRUCT_SECTION_FOREACH(zmk_rpc_subsystem, sub) {
        if (sub->subsystem_choice == choice) {
            return sub;
        }
    }

    return NULL;
}

zmk_studio_Response zmk_rpc_subsystem_delegate_to_subs(const struct zmk_rpc_subsystem *subsys,
                                                       const zmk_studio_Request *req,
                                                       uint8_t which_req) {
    LOG_DBG("Got subsystem func for %d", subsys->subsystem_choice);

    for (int i = subsys->handlers_start_index; i <= subsys->handlers_end_index; i++) {
        struct zmk_rpc_subsystem_handler *sub_handler;
        STRUCT_SECTION_GET(zmk_rpc_subsystem_handler, i, &sub_handler);
        if (sub_handler->request_choice == which_req) {
            if (sub_handler->security == ZMK_STUDIO_RPC_HANDLER_SECURED &&
                zmk_studio_core_get_lock_state() != ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED) {
                return ZMK_RPC_RESPONSE(meta, simple_error,
                                        zmk_meta_ErrorConditions_UNLOCK_REQUIRED);
            }
            return sub_handler->func(req);
        }
    }
    LOG_ERR("No handler func found for %d (subsystem %d)", which_req, subsys->subsystem_choice);
    return ZMK_RPC_RESPONSE(meta, simple_error, zmk_meta_ErrorConditions_RPC_NOT_FOUND);
}

static zmk_studio_Response handle_request(const zmk_studio_Request *req) {
    zmk_studio_core_reschedule_lock_timeout();
    struct zmk_rpc_subsystem *sub = find_subsystem_for_choice(req->which_subsystem);
    if (!sub) {
        LOG_WRN("No subsystem found for choice %d", req->which_subsystem);
        return ZMK_RPC_RESPONSE(meta, simple_error, zmk_meta_ErrorConditions_RPC_NOT_FOUND);
    }

    zmk_studio_Response resp = sub->func(sub, req);
    resp.type.request_response.request_id = req->request_id;

    return resp;
}

RING_BUF_DECLARE(rpc_rx_buf, CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE);
RING_BUF_DECLARE(rpc_tx_buf, CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE);

static K_SEM_DEFINE(rpc_rx_sem, 0, 1);
static K_SEM_DEFINE(rpc_tx_sem, 0, 1);

static K_MUTEX_DEFINE(rpc_transport_mutex);
static K_MUTEX_DEFINE(rpc_tx_mutex);
static struct zmk_rpc_transport *selected_transport;
static struct k_spinlock rpc_io_lock;
static uint32_t rpc_session;
static atomic_t failed_tx_session;
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_ZMK_STUDIO_TRANSPORT_UART)
static uint32_t selected_usb_connection;
#endif

// RPC thread state
static uint32_t request_session;
static void (*response_action)(bool);
#define RPC_ACTION_TIMEOUT_MS 15000

static struct {
    void (*complete)(bool);
    uint32_t session;
    uint32_t token;
    int64_t deadline;
} pending_action;
static uint32_t next_action_token;

static void finish_action_locked(bool confirmed) {
    if (pending_action.complete) {
        void (*complete)(bool) = pending_action.complete;
        pending_action.complete = NULL;
        complete(confirmed);
    }
}

static void expire_action(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&rpc_transport_mutex, K_FOREVER);
    if (pending_action.complete && k_uptime_get() >= pending_action.deadline) {
        finish_action_locked(false);
    }
    k_mutex_unlock(&rpc_transport_mutex);
}

static K_WORK_DELAYABLE_DEFINE(action_timeout, expire_action);

struct rpc_read_context {
    uint32_t session;
    enum studio_framing_state framing;
};

static uint32_t current_session_locked(void) {
    if (!selected_transport) {
        return 0;
    }
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_ZMK_STUDIO_TRANSPORT_UART)
    if (selected_transport->transport == ZMK_TRANSPORT_USB &&
        selected_usb_connection != zmk_usb_get_connection_id()) {
        return 0;
    }
#endif
    return rpc_session;
}

static bool session_matches_locked(enum zmk_transport transport, uint32_t session) {
    return session && session == current_session_locked() &&
           selected_transport->transport == transport;
}

void zmk_rpc_transport_fail(enum zmk_transport transport, uint32_t session) {
    k_spinlock_key_t key = k_spin_lock(&rpc_io_lock);
    bool failed = session_matches_locked(transport, session);
    if (failed) {
        atomic_set(&failed_tx_session, session);
        ring_buf_reset(&rpc_tx_buf);
        ring_buf_reset(&rpc_rx_buf);
    }
    k_spin_unlock(&rpc_io_lock, key);
    if (failed) {
        k_sem_give(&rpc_tx_sem);
        k_sem_give(&rpc_rx_sem);
    }
}

static uint32_t current_session(void) {
    k_spinlock_key_t key = k_spin_lock(&rpc_io_lock);
    uint32_t session = current_session_locked();
    k_spin_unlock(&rpc_io_lock, key);
    return session;
}

static bool session_is_current(uint32_t session) {
    return session && session == current_session();
}

uint32_t zmk_rpc_transport_session(enum zmk_transport transport) {
    k_spinlock_key_t key = k_spin_lock(&rpc_io_lock);
    uint32_t session = session_matches_locked(transport, rpc_session) ? rpc_session : 0;
    k_spin_unlock(&rpc_io_lock, key);
    return session;
}

bool zmk_rpc_is_transport_active(enum zmk_transport transport) {
    return zmk_rpc_transport_session(transport) != 0;
}

bool zmk_rpc_request_is_transport(enum zmk_transport transport) {
    return request_session && request_session == zmk_rpc_transport_session(transport);
}

uint32_t zmk_rpc_rx_write(enum zmk_transport transport, uint32_t session,
                          const uint8_t *data, size_t size) {
    k_spinlock_key_t key = k_spin_lock(&rpc_io_lock);
    uint32_t written = 0;
    if (session_matches_locked(transport, session) &&
        session != (uint32_t)atomic_get(&failed_tx_session) &&
        size <= ring_buf_space_get(&rpc_rx_buf)) {
        written = ring_buf_put(&rpc_rx_buf, data, size);
    }
    k_spin_unlock(&rpc_io_lock, key);
    if (written) {
        k_sem_give(&rpc_rx_sem);
    }
    return written;
}

uint32_t zmk_rpc_tx_read(enum zmk_transport transport, uint32_t session,
                         uint8_t *data, size_t size) {
    k_spinlock_key_t key = k_spin_lock(&rpc_io_lock);
    uint32_t read = 0;
    if (session_matches_locked(transport, session) &&
        session != (uint32_t)atomic_get(&failed_tx_session)) {
        read = ring_buf_get(&rpc_tx_buf, data, size);
    }
    k_spin_unlock(&rpc_io_lock, key);
    if (read) {
        k_sem_give(&rpc_tx_sem);
    }
    return read;
}

static uint32_t reset_session_locked(void) {
    k_work_cancel_delayable(&action_timeout);
    finish_action_locked(false);
    k_spinlock_key_t key = k_spin_lock(&rpc_io_lock);
    selected_transport = NULL;
    atomic_clear(&failed_tx_session);
    if (++rpc_session == 0) {
        rpc_session++;
    }
    ring_buf_reset(&rpc_rx_buf);
    ring_buf_reset(&rpc_tx_buf);
    uint32_t session = rpc_session;
    k_spin_unlock(&rpc_io_lock, key);
    k_sem_give(&rpc_rx_sem);
    k_sem_give(&rpc_tx_sem);
    return session;
}

static void publish_transport_locked(struct zmk_rpc_transport *transport) {
    k_spinlock_key_t key = k_spin_lock(&rpc_io_lock);
    selected_transport = transport;
    k_spin_unlock(&rpc_io_lock, key);
}

uint32_t zmk_rpc_transport_reset(enum zmk_transport transport, uint32_t session) {
    k_mutex_lock(&rpc_transport_mutex, K_FOREVER);
    if (!session || zmk_rpc_transport_session(transport) != session) {
        k_mutex_unlock(&rpc_transport_mutex);
        return 0;
    }
    struct zmk_rpc_transport *selected = selected_transport;
    uint32_t next = reset_session_locked();
    publish_transport_locked(selected);
#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOCK_ON_DISCONNECT)
    zmk_studio_core_lock();
#endif
    k_mutex_unlock(&rpc_transport_mutex);
    return next;
}

static bool rpc_read_cb(pb_istream_t *stream, uint8_t *buf, size_t count) {
    struct rpc_read_context *context = stream->state;
    size_t offset = 0;

    while (offset < count) {
        uint8_t byte;
        k_spinlock_key_t key = k_spin_lock(&rpc_io_lock);
        uint32_t session = current_session_locked();
        if (context->session && (context->session != session ||
                                 session == (uint32_t)atomic_get(&failed_tx_session))) {
            k_spin_unlock(&rpc_io_lock, key);
            stream->bytes_left = 0;
            return false;
        }
        uint32_t len = session ? ring_buf_get(&rpc_rx_buf, &byte, 1) : 0;
        if (len) {
            context->session = session;
        }
        k_spin_unlock(&rpc_io_lock, key);
        if (!len) {
            k_sem_take(&rpc_rx_sem, K_FOREVER);
            continue;
        }
        if (studio_framing_process_byte(&context->framing, byte)) {
            if (buf) {
                buf[offset] = byte;
            }
            offset++;
        }
        if (context->framing == FRAMING_STATE_EOF) {
            stream->bytes_left = 0;
            return false;
        }
    }
    return true;
}

static bool needs_escape(uint8_t byte) {
    return byte == FRAMING_SOF || byte == FRAMING_EOF || byte == FRAMING_ESC;
}

BUILD_ASSERT(CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE >= 2);

struct rpc_write_context {
    const struct zmk_rpc_transport *transport;
    uint32_t session;
    bool notification;
    bool started;
    int err;
    size_t len;
    uint8_t data[MIN(64, CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE)];
};

static int rpc_tx_flush(struct rpc_write_context *context) {
    int64_t deadline = k_uptime_get() + 5000;
    for (;;) {
        k_spinlock_key_t key = k_spin_lock(&rpc_io_lock);
        if (context->session != current_session_locked() ||
            context->session == (uint32_t)atomic_get(&failed_tx_session)) {
            k_spin_unlock(&rpc_io_lock, key);
            return -ENOTCONN;
        }
        if (context->len <= ring_buf_space_get(&rpc_tx_buf)) {
            ring_buf_put(&rpc_tx_buf, context->data, context->len);
            k_spin_unlock(&rpc_io_lock, key);
            context->started = true;
            context->len = 0;
            if (context->transport->tx_notify) {
                context->transport->tx_notify(context->session);
            }
            return 0;
        }
        k_spin_unlock(&rpc_io_lock, key);
        if (context->notification) {
            return -EAGAIN;
        }
        int64_t remaining = deadline - k_uptime_get();
        if (remaining <= 0 || k_sem_take(&rpc_tx_sem, K_MSEC(remaining))) {
            return -ETIMEDOUT;
        }
    }
}

static bool rpc_tx_buffer_write(pb_ostream_t *stream, const uint8_t *buf, size_t count) {
    struct rpc_write_context *context = stream->state;
    for (size_t i = 0; i < count; i++) {
        bool escape = needs_escape(buf[i]);
        if (context->len + (escape ? 2 : 1) > sizeof(context->data)) {
            context->err = rpc_tx_flush(context);
            if (context->err) {
                return false;
            }
        }
        if (escape) {
            context->data[context->len++] = FRAMING_ESC;
        }
        context->data[context->len++] = buf[i];
    }
    return true;
}

static bool escaped_size(pb_ostream_t *stream, const uint8_t *buf, size_t count) {
    size_t *size = stream->state;
    for (size_t i = 0; i < count; i++) {
        *size += needs_escape(buf[i]) ? 2 : 1;
    }
    return true;
}

static int send_response(const zmk_studio_Response *resp, uint32_t session) {
    bool notification = resp->which_type == zmk_studio_Response_notification_tag;
    if (k_mutex_lock(&rpc_tx_mutex, notification ? K_NO_WAIT : K_MSEC(500))) {
        return -EAGAIN;
    }

    struct rpc_write_context context = {
        .session = session,
        .notification = notification,
        .data = {FRAMING_SOF},
        .len = 1,
    };
    k_spinlock_key_t key = k_spin_lock(&rpc_io_lock);
    if (session && session == current_session_locked()) {
        context.transport = selected_transport;
    }
    size_t free = ring_buf_space_get(&rpc_tx_buf);
    size_t queued = ring_buf_size_get(&rpc_tx_buf);
    k_spin_unlock(&rpc_io_lock, key);

    int err;
    if (!context.transport || session == (uint32_t)atomic_get(&failed_tx_session)) {
        err = -ENOTCONN;
        goto done;
    }
    if (notification) {
        size_t size = 2;
        pb_ostream_t sizing = {escaped_size, &size, SIZE_MAX, 0};
        if (!pb_encode(&sizing, &zmk_studio_Response_msg, resp)) {
            err = -EINVAL;
            goto done;
        }
        if (size > free || queued > 128) {
            err = -EAGAIN;
            goto done;
        }
    }

    pb_ostream_t stream = {rpc_tx_buffer_write, &context, SIZE_MAX, 0};
    if (!pb_encode(&stream, &zmk_studio_Response_msg, resp)) {
        err = context.err ? context.err : -EINVAL;
        goto done;
    }
    if (context.len == sizeof(context.data)) {
        err = rpc_tx_flush(&context);
        if (err) {
            goto done;
        }
    }
    context.data[context.len++] = FRAMING_EOF;
    err = rpc_tx_flush(&context);

done:
    if (err && context.started && session_is_current(session)) {
        zmk_rpc_transport_fail(context.transport->transport, session);
        LOG_ERR("Incomplete RPC frame");
    }
    k_mutex_unlock(&rpc_tx_mutex);
    return err;
}

bool zmk_rpc_defer_action(const zmk_studio_Request *request, void (*complete)(bool)) {
    if (!request->supports_action_confirmation || response_action) {
        return false;
    }
    k_mutex_lock(&rpc_transport_mutex, K_FOREVER);
    bool available = !pending_action.complete && session_is_current(request_session);
    if (available) {
        response_action = complete;
    }
    k_mutex_unlock(&rpc_transport_mutex);
    return available;
}

static void confirm_action(const zmk_studio_Request *request, uint32_t session) {
    k_mutex_lock(&rpc_transport_mutex, K_FOREVER);
    if (pending_action.complete && pending_action.session == session &&
        session != (uint32_t)atomic_get(&failed_tx_session) &&
        pending_action.token == request->subsystem.confirm_action &&
        k_uptime_get() < pending_action.deadline && session_is_current(session) &&
        zmk_studio_core_get_lock_state() == ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED) {
        k_work_cancel_delayable(&action_timeout);
        finish_action_locked(true);
    }
    k_mutex_unlock(&rpc_transport_mutex);
}

static int send_request_response(zmk_studio_Response *resp, uint32_t session) {
    bool deferred = response_action != NULL;
    if (deferred) {
        k_mutex_lock(&rpc_transport_mutex, K_FOREVER);
        if (!session_is_current(session)) {
            response_action(false);
            k_mutex_unlock(&rpc_transport_mutex);
            return -ENOTCONN;
        }
        if (++next_action_token == 0) {
            next_action_token++;
        }
        pending_action.complete = response_action;
        pending_action.session = session;
        pending_action.token = next_action_token;
        pending_action.deadline = k_uptime_get() + RPC_ACTION_TIMEOUT_MS;
        resp->type.request_response.action_token = pending_action.token;
        k_work_reschedule(&action_timeout, K_MSEC(RPC_ACTION_TIMEOUT_MS));
        k_mutex_unlock(&rpc_transport_mutex);
    }

    int err = send_response(resp, session);
    if (err && deferred) {
        k_mutex_lock(&rpc_transport_mutex, K_FOREVER);
        if (pending_action.session == session) {
            k_work_cancel_delayable(&action_timeout);
            finish_action_locked(false);
        }
        k_mutex_unlock(&rpc_transport_mutex);
    }
    return err;
}

static void rpc_main(void) {
    for (;;) {
        struct rpc_read_context context = {.framing = FRAMING_STATE_IDLE};
        pb_istream_t stream = {&rpc_read_cb, &context, SIZE_MAX};
        zmk_studio_Request req = zmk_studio_Request_init_zero;
#if IS_ENABLED(CONFIG_THREAD_ANALYZER)
        thread_analyzer_print();
#endif // IS_ENABLED(CONFIG_THREAD_ANALYZER)
        bool status = pb_decode(&stream, &zmk_studio_Request_msg, &req);

        if (status && session_is_current(context.session) &&
            context.session != (uint32_t)atomic_get(&failed_tx_session)) {
            if (req.which_subsystem == zmk_studio_Request_confirm_action_tag) {
                confirm_action(&req, context.session);
                continue;
            }
            request_session = context.session;
            response_action = NULL;
            zmk_studio_Response resp = handle_request(&req);

            int err = send_request_response(&resp, context.session);
#if IS_ENABLED(CONFIG_THREAD_ANALYZER)
            thread_analyzer_print();
#endif // IS_ENABLED(CONFIG_THREAD_ANALYZER)
            if (err < 0) {
                LOG_ERR("Failed to send the RPC response %d", err);
            }
        } else {
            LOG_DBG("Decode failed or RPC session changed");
        }
        request_session = 0;
    }
}

K_THREAD_DEFINE(studio_rpc_thread, CONFIG_ZMK_STUDIO_RPC_THREAD_STACK_SIZE, rpc_main, NULL, NULL,
                NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

static void refresh_selected_transport(void) {
    enum zmk_transport transport = zmk_endpoints_selected().transport;
    struct zmk_rpc_transport *next = NULL;
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_ZMK_STUDIO_TRANSPORT_UART)
    uint32_t usb_connection = zmk_usb_get_connection_id();
    bool usb_ready = zmk_usb_is_hid_ready() &&
                     usb_connection == zmk_usb_get_connection_id();
    if (usb_ready) {
        transport = ZMK_TRANSPORT_USB;
    }
#endif
    STRUCT_SECTION_FOREACH(zmk_rpc_transport, t) {
        if (t->transport == transport) {
            next = t;
            break;
        }
    }
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_ZMK_STUDIO_TRANSPORT_UART)
    if (transport == ZMK_TRANSPORT_USB && !usb_ready) {
        next = NULL;
    }
#endif

    k_mutex_lock(&rpc_transport_mutex, K_FOREVER);
    bool unchanged = selected_transport == next;
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_ZMK_STUDIO_TRANSPORT_UART)
    if (next && transport == ZMK_TRANSPORT_USB) {
        unchanged &= selected_usb_connection == usb_connection;
    }
#endif
    if (unchanged) {
        k_mutex_unlock(&rpc_transport_mutex);
        return;
    }
    if (selected_transport && selected_transport->rx_stop) {
        selected_transport->rx_stop();
    }
    bool had_transport = selected_transport != NULL;
    reset_session_locked();
#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOCK_ON_DISCONNECT)
    if (had_transport) {
        zmk_studio_core_lock();
    }
#else
    ARG_UNUSED(had_transport);
#endif
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_ZMK_STUDIO_TRANSPORT_UART)
    k_spinlock_key_t key = k_spin_lock(&rpc_io_lock);
    selected_usb_connection = usb_connection;
    k_spin_unlock(&rpc_io_lock, key);
#endif
    if (next) {
        publish_transport_locked(next);
        int ret = next->rx_start ? next->rx_start() : 0;
        if (ret < 0) {
            LOG_ERR("Failed to start RPC transport: %d", ret);
            reset_session_locked();
        }
    }
    k_mutex_unlock(&rpc_transport_mutex);
}

static int zmk_rpc_init(void) {
    int prev_choice = -1;
    struct zmk_rpc_subsystem *prev_sub = NULL;
    int i = 0;

    STRUCT_SECTION_FOREACH(zmk_rpc_subsystem_handler, handler) {
        struct zmk_rpc_subsystem *sub = find_subsystem_for_choice(handler->subsystem_choice);

        __ASSERT(sub != NULL, "RPC Handler for unknown subsystem choice %d",
                 handler->subsystem_choice);

        if (prev_choice < 0) {
            sub->handlers_start_index = i;
        } else if ((prev_choice != handler->subsystem_choice) && prev_sub) {
            prev_sub->handlers_end_index = i - 1;
            sub->handlers_start_index = i;
        }

        prev_choice = handler->subsystem_choice;
        prev_sub = sub;
        i++;
    }

    if (prev_sub) {
        prev_sub->handlers_end_index = i - 1;
    }

    refresh_selected_transport();

    return 0;
}

SYS_INIT(zmk_rpc_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int studio_rpc_listener_cb(const zmk_event_t *eh) {
    bool transport_changed = as_zmk_endpoint_changed(eh) != NULL;
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_ZMK_STUDIO_TRANSPORT_UART)
    transport_changed |= as_zmk_usb_conn_state_changed(eh) != NULL;
#endif
    if (transport_changed) {
        refresh_selected_transport();
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct zmk_studio_rpc_notification *rpc_notify = as_zmk_studio_rpc_notification(eh);
    if (rpc_notify) {
        zmk_studio_Response resp = zmk_studio_Response_init_zero;
        resp.which_type = zmk_studio_Response_notification_tag;
        resp.type.notification = rpc_notify->notification;
        send_response(&resp, current_session());
        return ZMK_EV_EVENT_BUBBLE;
    }

    zmk_studio_Notification n = zmk_studio_Notification_init_zero;
    STRUCT_SECTION_FOREACH(zmk_rpc_event_mapper, mapper) {
        int ret = mapper->func(eh, &n);
        if (ret >= 0) {
            raise_zmk_studio_rpc_notification(
                (struct zmk_studio_rpc_notification){.notification = n});
            break;
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(studio_rpc, studio_rpc_listener_cb);
ZMK_SUBSCRIPTION(studio_rpc, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_ZMK_USB) && IS_ENABLED(CONFIG_ZMK_STUDIO_TRANSPORT_UART)
ZMK_SUBSCRIPTION(studio_rpc, zmk_usb_conn_state_changed);
#endif
ZMK_SUBSCRIPTION(studio_rpc, zmk_studio_rpc_notification);
