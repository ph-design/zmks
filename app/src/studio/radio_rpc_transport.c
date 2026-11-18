/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>
#include <zmk/esb.h>
#include <zmk/2g4.h>
#include <zmk/2g4_crypto.h>
#include <zmk/studio/radio.h>
#include <zmk/studio/radio_link.h>
#include <zmk/studio/rpc.h>

struct radio_message {
    uint32_t rpc_session;
    uint8_t len, data[20];
};
K_MSGQ_DEFINE(studio_packets, sizeof(struct radio_message), 8, 4);
static struct sr_link link;
static bool active;
static int64_t last_rx;
static uint32_t transport_session;
static atomic_t ready;
void studio_radio_status(char *out, size_t size) {
    snprintk(out, size,
             "STUDIO_2G4=1\r\nSTUDIO_ACTIVE=%d\r\nSTUDIO_PORT=RADIO\r\nBLE_MOUSE=0\r\n",
             active);
}

void studio_radio_started(void) { atomic_set(&ready, 1); }
void studio_radio_received(const uint8_t *data, size_t len) {
    if (!sr_valid(data, len))
        return;
    uint32_t session = zmk_rpc_transport_session(ZMK_TRANSPORT_2G4);
    if (!session) {
        return;
    }
    struct radio_message m = {.rpc_session = session, .len = len};
    memcpy(m.data, data, len);
    k_msgq_put(&studio_packets, &m, K_NO_WAIT);
}

static atomic_t studio_selected;
static int rx_start(void) {
    atomic_set(&studio_selected, 1);
    return 0;
}
static int rx_stop(void) {
    atomic_set(&studio_selected, 0);
    return 0;
}
ZMK_RPC_TRANSPORT(radio, ZMK_TRANSPORT_2G4, rx_start, rx_stop, NULL);
static bool can_receive(void) { return atomic_get(&studio_selected); }
static void abort_rpc_stream(void) {
    link.pending_len = 0;
    transport_session = zmk_rpc_transport_reset(ZMK_TRANSPORT_2G4, transport_session);
}

static uint32_t read_input(uint8_t *data, uint32_t size) {
    return zmk_rpc_tx_read(ZMK_TRANSPORT_2G4, transport_session, data, size);
}

static void emit(uint8_t flags) {
    struct zmk_esb_payload p = {.pipe = 0, .noack = false};
    size_t n = sr_encode(&link, flags, p.data);
    int encrypted = zmk_2g4_crypto_encrypt(p.data, n, sizeof(p.data));
    if (encrypted <= 0)
        return;
    p.length = encrypted;
    zmk_esb_write_payload(&p);
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
            k_msleep(10);
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
            transport_session = selected;
            k_msgq_purge(&studio_packets);
        }
        if (!selected || !zmk_2g4_is_ready()) {
            if (active) {
                active = false;
                abort_rpc_stream();
            }
            k_msleep(20);
            continue;
        }
        if (active && k_uptime_get() - last_rx > 3000) {
            active = false;
            abort_rpc_stream();
        }
        // Drain unsent replies while the radio session is closed
        if (!active && can_receive()) {
            uint8_t drop[32];
            read_input(drop, sizeof(drop));
        }
        struct radio_message message;
        while (k_msgq_get(&studio_packets, &message, K_NO_WAIT) == 0)
            process(&message);
        if (active && transport_session == zmk_rpc_transport_session(ZMK_TRANSPORT_2G4)) {
            if (!link.pending_len) {
                link.pending_len = read_input(link.pending, SR_DATA);
            }
            emit(0);
        }
        k_msleep(active ? 5 : 20);
    }
}
K_THREAD_DEFINE(studio_radio_thread, 2048, radio_main, NULL, NULL, NULL, 5, 0, 50);
