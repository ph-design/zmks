/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zmk/2g4.h>
#include <zmk/studio/radio.h>
#include <zmk/studio/radio_link.h>
#include <zmk/studio/rpc.h>

struct radio_message {
    uint32_t rpc_session;
    uint8_t len, data[20];
};
K_MSGQ_DEFINE(studio_packets, sizeof(struct radio_message), 8, 4);
static K_SEM_DEFINE(radio_wake, 0, 1);
static struct sr_link link;
static bool active;
static int64_t last_rx;
#define STUDIO_TX_INTERVAL_MS 2
#define STUDIO_POLL_MS 20

static int64_t next_tx, next_poll, retry_at;
static atomic_t tx_waiting;
static uint8_t ack_sent;
static uint32_t transport_session;
static atomic_t ready;
void studio_radio_started(void) {
    atomic_set(&ready, 1);
    k_sem_give(&radio_wake);
}
void studio_radio_tx_available(void) {
    if (atomic_cas(&tx_waiting, 1, 0)) {
        k_sem_give(&radio_wake);
    }
}

void studio_radio_received(const uint8_t *data, size_t len) {
    if (!sr_valid(data, len))
        return;
    uint32_t session = zmk_rpc_transport_session(ZMK_TRANSPORT_2G4);
    if (!session) {
        return;
    }
    struct radio_message m = {.rpc_session = session, .len = len};
    memcpy(m.data, data, len);
    if (!k_msgq_put(&studio_packets, &m, K_NO_WAIT)) {
        k_sem_give(&radio_wake);
    }
}

static atomic_t studio_selected;
static int rx_start(void) {
    atomic_set(&studio_selected, 1);
    k_sem_give(&radio_wake);
    return 0;
}
static int rx_stop(void) {
    atomic_set(&studio_selected, 0);
    k_sem_give(&radio_wake);
    return 0;
}
static void tx_notify(uint32_t session) {
    ARG_UNUSED(session);
    k_sem_give(&radio_wake);
}

ZMK_RPC_TRANSPORT(radio, ZMK_TRANSPORT_2G4, rx_start, rx_stop, tx_notify);
static bool can_receive(void) { return atomic_get(&studio_selected); }
static void abort_rpc_stream(void) {
    atomic_clear(&tx_waiting);
    link.pending_len = 0;
    transport_session = zmk_rpc_transport_reset(ZMK_TRANSPORT_2G4, transport_session);
}

static uint32_t read_input(uint8_t *data, uint32_t size) {
    return zmk_rpc_tx_read(ZMK_TRANSPORT_2G4, transport_session, data, size);
}

static int emit(uint8_t flags) {
    uint8_t data[SR_HEADER + SR_DATA];
    size_t len = sr_encode(&link, flags, data);
    return zmk_2g4_send_background(data, len);
}
static void process(struct radio_message *m) {
    uint32_t session = sr_session(m->data);
    if (!zmk_2g4_is_ready() || !can_receive() || m->rpc_session != transport_session ||
        transport_session != zmk_rpc_transport_session(ZMK_TRANSPORT_2G4))
        return;
    if ((m->data[1] & SR_OPEN) && (!link.session || (int32_t)(session - link.session) > 0)) {
        abort_rpc_stream();
        if (!transport_session) {
            return;
        }
        sr_reset(&link, session);
        ack_sent = link.rx;
        next_tx = next_poll = 0;
        active = true;
        k_msgq_purge(&studio_packets);
    }
    if (!active || session != link.session) {
        if (!active && session == link.session) {
            link.pending_len = 0;
            emit(SR_CLOSE);
        }
        return;
    }
    last_rx = k_uptime_get();
    if (m->data[1] & SR_CLOSE) {
        active = false;
        abort_rpc_stream();
        return;
    }
    sr_ack(&link, m->data);
    size_t len = m->len - SR_HEADER;
    if (can_receive() && sr_data_ready(&link, m->data, m->len) &&
        zmk_rpc_rx_write(ZMK_TRANSPORT_2G4, transport_session,
                         m->data + SR_HEADER, len) == len) {
        link.rx++;
    }
}
static void radio_main(void) {
    for (;;) {
        if (!atomic_get(&ready)) {
            k_sem_take(&radio_wake, K_FOREVER);
            continue;
        }
        uint32_t selected = can_receive()
                                ? zmk_rpc_transport_session(ZMK_TRANSPORT_2G4) : 0;
        if (selected != transport_session) {
            link.pending_len = 0;
            if (active && zmk_2g4_is_ready()) {
                emit(SR_CLOSE);
            }
            active = false;
            atomic_clear(&tx_waiting);
            transport_session = selected;
            k_msgq_purge(&studio_packets);
        }
        if (!selected || !zmk_2g4_is_ready()) {
            if (active) {
                active = false;
                abort_rpc_stream();
            }
            k_sem_take(&radio_wake, K_MSEC(STUDIO_POLL_MS));
            continue;
        }
        if (active && (zmk_rpc_transport_failed(transport_session) ||
                       k_uptime_get() - last_rx > 3000)) {
            active = false;
            abort_rpc_stream();
            emit(SR_CLOSE);
        }
        // Drain unsent replies while the radio session is closed
        if (!active && can_receive()) {
            uint8_t drop[32];
            read_input(drop, sizeof(drop));
        }
        struct radio_message message;
        while (k_msgq_get(&studio_packets, &message, K_NO_WAIT) == 0)
            process(&message);
        int64_t now = k_uptime_get();
        int64_t wake_at = now + STUDIO_POLL_MS;
        if (active && transport_session == zmk_rpc_transport_session(ZMK_TRANSPORT_2G4)) {
            if (!link.pending_len) {
                link.pending_len = read_input(link.pending, SR_DATA);
            }
            bool pending = link.pending_len || link.rx != ack_sent;
            bool waiting = atomic_get(&tx_waiting) && now < retry_at;
            if (!waiting && now >= next_tx && (pending || now >= next_poll)) {
                atomic_set(&tx_waiting, 1);
                int err = emit(0);
                if (err == -EBUSY || err == -ENOMEM) {
                    retry_at = now + STUDIO_POLL_MS;
                } else {
                    atomic_clear(&tx_waiting);
                    next_tx = now + (err ? STUDIO_POLL_MS : STUDIO_TX_INTERVAL_MS);
                    if (!err) {
                        ack_sent = link.rx;
                        next_poll = now + STUDIO_POLL_MS;
                    }
                    pending = link.pending_len || link.rx != ack_sent;
                }
            }
            int64_t send_at = pending ? next_tx : MAX(next_tx, next_poll);
            if (atomic_get(&tx_waiting)) {
                send_at = MAX(send_at, retry_at);
            }
            wake_at = MIN(wake_at, send_at);
        }
        k_sem_take(&radio_wake, K_TIMEOUT_ABS_MS(wake_at));
    }
}
K_THREAD_DEFINE(studio_radio_thread, 2048, radio_main, NULL, NULL, NULL, 5, 0, 50);
