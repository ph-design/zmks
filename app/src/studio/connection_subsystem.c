/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <string.h>

#include <zmk/studio/rpc.h>
#include <zmk/endpoints.h>
#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/usb_conn_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_2G4)
#include <zmk/2g4.h>
#include <zmk/2g4_crypto.h>
#endif

ZMK_RPC_SUBSYSTEM(connection)

#define CONNECTION_RESPONSE(type, ...) ZMK_RPC_RESPONSE(connection, type, __VA_ARGS__)

// Reply before switching transports.
#define CONN_SWITCH_DELAY_MS 200

// Notify after endpoint listeners update the RPC transport.
#define CONN_NOTIFY_DELAY_MS 40

#define CONN_MAX_BLE_PROFILES 5

enum conn_action {
    CONN_ACTION_NONE = 0,
    CONN_ACTION_SELECT_TRANSPORT,
    CONN_ACTION_TOGGLE_TRANSPORT,
    CONN_ACTION_SELECT_PROFILE,
    CONN_ACTION_UNPAIR_PROFILE,
    CONN_ACTION_UNPAIR_ALL,
};

static K_MUTEX_DEFINE(conn_lock);

// Radio teardown must not block the system workqueue.
static K_SEM_DEFINE(conn_switch_sem, 0, 1);
static struct k_thread conn_switch_thread;
K_THREAD_STACK_DEFINE(conn_switch_stack, 2048);
static enum conn_action conn_pending_action;
static enum zmk_transport conn_pending_transport;
static uint32_t conn_pending_profile;
static bool conn_switch_in_flight;

static zmk_connection_Transport transport_to_proto(enum zmk_transport transport) {
    switch (transport) {
    case ZMK_TRANSPORT_USB:
        return zmk_connection_Transport_ZMK_CONNECTION_TRANSPORT_USB;
    case ZMK_TRANSPORT_BLE:
        return zmk_connection_Transport_ZMK_CONNECTION_TRANSPORT_BLE;
#if IS_ENABLED(CONFIG_ZMK_2G4)
    case ZMK_TRANSPORT_2G4:
        return zmk_connection_Transport_ZMK_CONNECTION_TRANSPORT_2G4;
#endif
    }

    return zmk_connection_Transport_ZMK_CONNECTION_TRANSPORT_USB;
}

static enum zmk_transport transport_from_proto(zmk_connection_Transport transport) {
    switch (transport) {
    case zmk_connection_Transport_ZMK_CONNECTION_TRANSPORT_USB:
        return ZMK_TRANSPORT_USB;
    case zmk_connection_Transport_ZMK_CONNECTION_TRANSPORT_BLE:
        return ZMK_TRANSPORT_BLE;
    case zmk_connection_Transport_ZMK_CONNECTION_TRANSPORT_2G4:
#if IS_ENABLED(CONFIG_ZMK_2G4)
        return ZMK_TRANSPORT_2G4;
#else
        break;
#endif
    }

    return (enum zmk_transport)-1;
}

static bool conn_usb_ready(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_hid_ready();
#else
    return false;
#endif
}

static bool conn_usb_attached(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_powered();
#else
    return false;
#endif
}

// Caller must hold conn_lock.
static void fill_state(zmk_connection_State *state) {
    struct zmk_endpoint_instance instance = zmk_endpoints_selected();

    state->supports_usb = IS_ENABLED(CONFIG_ZMK_USB);
    state->supports_ble = IS_ENABLED(CONFIG_ZMK_BLE);
    state->supports_2g4 = IS_ENABLED(CONFIG_ZMK_2G4);

    state->active_transport = transport_to_proto(instance.transport);
    zmk_endpoint_instance_to_str(instance, state->endpoint, sizeof(state->endpoint));

    state->usb_attached = conn_usb_attached();
    state->usb_hid_ready = conn_usb_ready();
    state->radio_switching = conn_switch_in_flight;

#if IS_ENABLED(CONFIG_ZMK_BLE)
    int profile_count = MIN(ZMK_BLE_PROFILE_COUNT, CONN_MAX_BLE_PROFILES);
    int active_profile = zmk_ble_active_profile_index();

    state->ble_active_profile = (uint32_t)active_profile;
    state->ble_profile_count = (uint32_t)profile_count;
    state->ble_profiles_count = (pb_size_t)profile_count;

    for (int i = 0; i < profile_count; i++) {
        zmk_connection_BleProfile *entry = &state->ble_profiles[i];

        entry->index = (uint32_t)i;
        entry->open = zmk_ble_profile_is_open((uint8_t)i);
        entry->connected = zmk_ble_profile_is_connected((uint8_t)i);
        entry->active = (i == active_profile);

        if (!entry->open) {
            const bt_addr_le_t *peer = zmk_ble_profile_address((uint8_t)i);
            if (peer) {
                memcpy(entry->address.bytes, peer->a.val, sizeof(peer->a.val));
                entry->address.size = (pb_size_t)sizeof(peer->a.val);
                entry->address_type = peer->type;
            }
        }
    }

    state->ble_advertising = zmk_ble_is_advertising();
#endif

#if IS_ENABLED(CONFIG_ZMK_2G4)
    struct zmk_2g4_addr addr;

    zmk_2g4_addr_get(&addr);

    state->two_g4_ready = zmk_2g4_is_ready();
    state->two_g4_address_paired = zmk_2g4_addr_is_paired();
    state->two_g4_key_paired = zmk_2g4_crypto_has_paired_key();
    state->two_g4_channel = addr.rf_channel;

    // four base bytes followed by the pipe prefix, the same order the dongle's AT+PAIR takes
    memcpy(state->two_g4_address.bytes, addr.base, sizeof(addr.base));
    state->two_g4_address.bytes[sizeof(addr.base)] = addr.prefix;
    state->two_g4_address.size = (pb_size_t)(sizeof(addr.base) + 1);

#if IS_ENABLED(CONFIG_ZMK_BLE)
    state->preferred_wireless_2g4 =
        (zmk_endpoints_preferred_wireless_transport() == ZMK_TRANSPORT_2G4);
#else
    state->preferred_wireless_2g4 = true;
#endif
#endif
}

static zmk_connection_SwitchResult conn_check_target(enum zmk_transport target) {
    switch (target) {
    case ZMK_TRANSPORT_USB:
#if IS_ENABLED(CONFIG_ZMK_USB)
        if (!zmk_usb_is_powered()) {
            return zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_USB_REQUIRED;
        }
        // USB is usable only after HID enumeration completes.
        if (!conn_usb_ready()) {
            return zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_BUSY;
        }
        return zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_OK;
#else
        return zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_UNSUPPORTED;
#endif
    case ZMK_TRANSPORT_BLE:
        return IS_ENABLED(CONFIG_ZMK_BLE)
                   ? zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_OK
                   : zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_UNSUPPORTED;
#if IS_ENABLED(CONFIG_ZMK_2G4)
    case ZMK_TRANSPORT_2G4:
        return zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_OK;
#endif
    }

    return zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_INVALID;
}

static enum zmk_transport conn_next_toggle_target(void) {
    enum zmk_transport current = zmk_endpoints_selected().transport;

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4)
    switch (current) {
    case ZMK_TRANSPORT_USB:
        return ZMK_TRANSPORT_BLE;
    case ZMK_TRANSPORT_BLE:
        return ZMK_TRANSPORT_2G4;
    default:
        return conn_usb_ready() ? ZMK_TRANSPORT_USB : ZMK_TRANSPORT_BLE;
    }
#elif IS_ENABLED(CONFIG_ZMK_2G4)
    return (current == ZMK_TRANSPORT_2G4) ? ZMK_TRANSPORT_USB : ZMK_TRANSPORT_2G4;
#else
    return (current == ZMK_TRANSPORT_USB) ? ZMK_TRANSPORT_BLE : ZMK_TRANSPORT_USB;
#endif
}

#if IS_ENABLED(CONFIG_ZMK_BLE)
static bool conn_any_ble_bonded(void) {
    for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        if (!zmk_ble_profile_is_open((uint8_t)i)) {
            return true;
        }
    }

    return false;
}
#endif

static zmk_connection_SwitchResult conn_schedule(enum conn_action action,
                                                enum zmk_transport target, uint32_t profile,
                                                zmk_connection_State *snapshot) {
    zmk_connection_SwitchResult result = zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_OK;

    k_mutex_lock(&conn_lock, K_FOREVER);

    fill_state(snapshot);

    if (conn_switch_in_flight) {
        result = zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_BUSY;
        goto unlock;
    }

    switch (action) {
    case CONN_ACTION_SELECT_TRANSPORT:
        result = conn_check_target(target);
        break;
    case CONN_ACTION_TOGGLE_TRANSPORT:
        target = conn_next_toggle_target();
        result = conn_check_target(target);
        break;
    case CONN_ACTION_SELECT_PROFILE:
    case CONN_ACTION_UNPAIR_PROFILE:
#if IS_ENABLED(CONFIG_ZMK_BLE)
        if (profile >= ZMK_BLE_PROFILE_COUNT) {
            result = zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_INVALID;
        }
#else
        result = zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_UNSUPPORTED;
#endif
        break;
    case CONN_ACTION_UNPAIR_ALL:
#if !IS_ENABLED(CONFIG_ZMK_BLE)
        result = zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_UNSUPPORTED;
#endif
        break;
    default:
        result = zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_INVALID;
        break;
    }

    if (result != zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_OK) {
        goto unlock;
    }

    // Selecting the current target succeeds without a radio switch.
    switch (action) {
    case CONN_ACTION_SELECT_TRANSPORT:
    case CONN_ACTION_TOGGLE_TRANSPORT:
        if (target == zmk_endpoints_selected().transport) {
            goto unlock;
        }
        break;
#if IS_ENABLED(CONFIG_ZMK_BLE)
    case CONN_ACTION_SELECT_PROFILE:
        if (profile == (uint32_t)zmk_ble_active_profile_index()) {
            goto unlock;
        }
        break;
    case CONN_ACTION_UNPAIR_PROFILE:
        if (zmk_ble_profile_is_open((uint8_t)profile)) {
            goto unlock;
        }
        break;
    case CONN_ACTION_UNPAIR_ALL:
        if (!conn_any_ble_bonded()) {
            goto unlock;
        }
        break;
#endif
    default:
        break;
    }

    conn_pending_action = action;
    conn_pending_transport = target;
    conn_pending_profile = profile;
    conn_switch_in_flight = true;
    k_sem_give(&conn_switch_sem);

    result = zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_PENDING;

unlock:
    k_mutex_unlock(&conn_lock);
    return result;
}

#if IS_ENABLED(CONFIG_ZMK_BLE)
// ble.c keeps its single-profile bond helper private.
extern void set_profile_address(uint8_t index, const bt_addr_le_t *addr);

static void conn_unpair_profile(uint32_t profile) {
    uint8_t index = (uint8_t)profile;

    if (zmk_ble_profile_is_open(index)) {
        return;
    }

    if (index == (uint8_t)zmk_ble_active_profile_index()) {
        zmk_ble_clear_bonds();
        return;
    }

    bt_addr_le_t *peer = zmk_ble_profile_address(index);
    if (!peer) {
        LOG_WRN("No address slot for BLE profile %d", index);
        return;
    }

    int err = bt_unpair(BT_ID_DEFAULT, peer);
    if (err < 0 && err != -ENOENT) {
        LOG_WRN("bt_unpair for profile %d failed: %d", index, err);
    }

    set_profile_address(index, BT_ADDR_LE_ANY);
}

static void conn_unpair_all(void) {
    zmk_ble_clear_all_bonds();
}
#endif

static void fill_state_changed(zmk_connection_StateChanged *changed) {
    k_mutex_lock(&conn_lock, K_FOREVER);

    changed->active_transport = transport_to_proto(zmk_endpoints_selected().transport);
    changed->usb_attached = conn_usb_attached();
    changed->radio_switching = conn_switch_in_flight;
#if IS_ENABLED(CONFIG_ZMK_BLE)
    changed->ble_active_profile = (uint32_t)zmk_ble_active_profile_index();
    changed->ble_connected = zmk_ble_active_profile_is_connected();
#endif
#if IS_ENABLED(CONFIG_ZMK_2G4)
    changed->two_g4_ready = zmk_2g4_is_ready();
#endif

    k_mutex_unlock(&conn_lock);
}

static void raise_state_changed(void) {
    zmk_connection_StateChanged changed = zmk_connection_StateChanged_init_zero;

    fill_state_changed(&changed);
    raise_zmk_studio_rpc_notification(
        (struct zmk_studio_rpc_notification){.notification = ZMK_RPC_NOTIFICATION(
                                                 connection, connection_state_changed, changed)});
}

static void conn_switch_run(void) {
    enum conn_action action;
    enum zmk_transport transport;
    uint32_t profile;
    int err = 0;

    k_mutex_lock(&conn_lock, K_FOREVER);
    action = conn_pending_action;
    transport = conn_pending_transport;
    profile = conn_pending_profile;
    conn_pending_action = CONN_ACTION_NONE;
    k_mutex_unlock(&conn_lock);

    switch (action) {
    case CONN_ACTION_SELECT_TRANSPORT:
    case CONN_ACTION_TOGGLE_TRANSPORT: {
#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4)
        if (action == CONN_ACTION_TOGGLE_TRANSPORT) {
            transport = conn_next_toggle_target();
        }
#endif
        if (action == CONN_ACTION_TOGGLE_TRANSPORT) {
            err = zmk_endpoints_toggle_transport();
        } else {
            err = zmk_endpoints_select_transport(transport);
        }
        break;
    }
#if IS_ENABLED(CONFIG_ZMK_BLE)
    case CONN_ACTION_SELECT_PROFILE:
        err = zmk_ble_prof_select((uint8_t)profile);
        break;
    case CONN_ACTION_UNPAIR_PROFILE:
        conn_unpair_profile(profile);
        break;
    case CONN_ACTION_UNPAIR_ALL:
        conn_unpair_all();
        break;
#endif
    default:
        break;
    }

    if (err < 0) {
        LOG_ERR("Connection action %d failed: %d", action, err);
    }

    k_mutex_lock(&conn_lock, K_FOREVER);
    conn_switch_in_flight = false;
    k_mutex_unlock(&conn_lock);

    raise_state_changed();
}

// Run queued actions after the RPC reply.
static void conn_switch_thread_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (;;) {
        k_sem_take(&conn_switch_sem, K_FOREVER);
        k_msleep(CONN_SWITCH_DELAY_MS);
        conn_switch_run();
    }
}

static zmk_studio_Response get_state(const zmk_studio_Request *req) {
    LOG_DBG("");

    zmk_connection_State state = zmk_connection_State_init_zero;

    k_mutex_lock(&conn_lock, K_FOREVER);
    fill_state(&state);
    k_mutex_unlock(&conn_lock);

    return CONNECTION_RESPONSE(get_state, state);
}

static zmk_studio_Response select_transport(const zmk_studio_Request *req) {
    LOG_DBG("");

    const zmk_connection_SelectTransportRequest *select =
        &req->subsystem.connection.request_type.select_transport;

    zmk_connection_State snapshot = zmk_connection_State_init_zero;
    zmk_connection_SwitchResult result = conn_schedule(
        CONN_ACTION_SELECT_TRANSPORT, transport_from_proto(select->transport), 0, &snapshot);

    zmk_connection_SelectTransportResponse resp = zmk_connection_SelectTransportResponse_init_zero;
    resp.result = result;
    resp.has_state = true;
    resp.state = snapshot;

    return CONNECTION_RESPONSE(select_transport, resp);
}

static zmk_studio_Response toggle_transport(const zmk_studio_Request *req) {
    LOG_DBG("");

    zmk_connection_State snapshot = zmk_connection_State_init_zero;
    zmk_connection_SwitchResult result =
        conn_schedule(CONN_ACTION_TOGGLE_TRANSPORT, ZMK_TRANSPORT_USB, 0, &snapshot);

    zmk_connection_SelectTransportResponse resp = zmk_connection_SelectTransportResponse_init_zero;
    resp.result = result;
    resp.has_state = true;
    resp.state = snapshot;

    return CONNECTION_RESPONSE(toggle_transport, resp);
}

static zmk_studio_Response select_ble_profile(const zmk_studio_Request *req) {
    LOG_DBG("");

    uint32_t index = req->subsystem.connection.request_type.select_ble_profile.index;

    zmk_connection_State snapshot = zmk_connection_State_init_zero;
    zmk_connection_SwitchResult result =
        conn_schedule(CONN_ACTION_SELECT_PROFILE, ZMK_TRANSPORT_USB, index, &snapshot);

    if (result != zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_OK &&
        result != zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_PENDING) {
        LOG_WRN("BLE profile select %u rejected (%d)", index, result);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    return CONNECTION_RESPONSE(select_ble_profile, snapshot);
}

static zmk_studio_Response unpair_ble_profile(const zmk_studio_Request *req) {
    LOG_DBG("");

    uint32_t index = req->subsystem.connection.request_type.unpair_ble_profile.index;

    zmk_connection_State snapshot = zmk_connection_State_init_zero;
    zmk_connection_SwitchResult result =
        conn_schedule(CONN_ACTION_UNPAIR_PROFILE, ZMK_TRANSPORT_USB, index, &snapshot);

    if (result != zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_OK &&
        result != zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_PENDING) {
        LOG_WRN("BLE profile unpair %u rejected (%d)", index, result);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    return CONNECTION_RESPONSE(unpair_ble_profile, snapshot);
}

static zmk_studio_Response unpair_all_ble(const zmk_studio_Request *req) {
    LOG_DBG("");

    zmk_connection_State snapshot = zmk_connection_State_init_zero;
    zmk_connection_SwitchResult result =
        conn_schedule(CONN_ACTION_UNPAIR_ALL, ZMK_TRANSPORT_USB, 0, &snapshot);

    if (result != zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_OK &&
        result != zmk_connection_SwitchResult_ZMK_CONNECTION_SWITCH_RESULT_PENDING) {
        LOG_WRN("Unpairing all BLE profiles rejected (%d)", result);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    return CONNECTION_RESPONSE(unpair_all_ble, snapshot);
}

ZMK_RPC_SUBSYSTEM_HANDLER(connection, get_state, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(connection, select_transport, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(connection, select_ble_profile, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(connection, unpair_ble_profile, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(connection, unpair_all_ble, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(connection, toggle_transport, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int connection_event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) {
    const struct zmk_endpoint_changed *endpoint_ev = as_zmk_endpoint_changed(eh);
    const struct zmk_usb_conn_state_changed *usb_ev = as_zmk_usb_conn_state_changed(eh);
#if IS_ENABLED(CONFIG_ZMK_BLE)
    const struct zmk_ble_active_profile_changed *profile_ev = as_zmk_ble_active_profile_changed(eh);
#endif

    if (!endpoint_ev && !usb_ev
#if IS_ENABLED(CONFIG_ZMK_BLE)
        && !profile_ev
#endif
    ) {
        return -ENOTSUP;
    }

    zmk_connection_StateChanged changed = zmk_connection_StateChanged_init_zero;

    fill_state_changed(&changed);

    *n = ZMK_RPC_NOTIFICATION(connection, connection_state_changed, changed);

    return 0;
}

// Use a separate listener and defer notifications until RPC transport selection is updated.
static struct k_work_delayable connection_notify_work;

static void connection_notify_handler(struct k_work *work) { raise_state_changed(); }

static int connection_event_listener_cb(const zmk_event_t *eh) {
    zmk_studio_Notification n = zmk_studio_Notification_init_zero;

    if (connection_event_mapper(eh, &n) < 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    k_work_reschedule(&connection_notify_work, K_MSEC(CONN_NOTIFY_DELAY_MS));

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(connection_events, connection_event_listener_cb);
ZMK_SUBSCRIPTION(connection_events, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(connection_events, zmk_usb_conn_state_changed);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(connection_events, zmk_ble_active_profile_changed);
#endif

static int zmk_connection_studio_init(void) {
    k_work_init_delayable(&connection_notify_work, connection_notify_handler);

    k_thread_create(&conn_switch_thread, conn_switch_stack,
                    K_THREAD_STACK_SIZEOF(conn_switch_stack), conn_switch_thread_fn, NULL, NULL,
                    NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
    k_thread_name_set(&conn_switch_thread, "studio_conn");

    return 0;
}

SYS_INIT(zmk_connection_studio_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
