/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/random/random.h>
#include <zmk/esb.h>
#include <zmk/2g4.h>
#include <zmk/2g4_crypto.h>
#include <zmk/studio/radio.h>
#include <zmk/studio/radio_link.h>
#include <zmk/studio/rpc.h>
#include <zmk/studio/core.h>

struct radio_message {
    uint8_t len, data[20];
};
K_MSGQ_DEFINE(studio_packets, sizeof(struct radio_message), 8, 4);
static struct sr_link link;
static bool active, synchronized;
static int64_t last_rx;
static atomic_t ready;
void studio_radio_status(char *out, size_t size) {
    snprintk(out, size,
             "STUDIO_2G4=1\r\nSTUDIO_ACTIVE=%d\r\nSTUDIO_PORT=RADIO\r\nBLE_MOUSE=0\r\n",
             active && synchronized);
}

void studio_radio_started(void) { atomic_set(&ready, 1); }
void studio_radio_received(const uint8_t *data, size_t len) {
    if (!sr_valid(data, len))
        return;
    struct radio_message m = {.len = len};
    memcpy(m.data, data, len);
    // Overflow is recovered by stop-and-wait retransmission with a fresh CCM counter.
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
static void tx_notify(struct ring_buf *buf, size_t added, bool done, void *arg) {}
ZMK_RPC_TRANSPORT(radio, ZMK_TRANSPORT_2G4, rx_start, rx_stop, NULL, tx_notify);
static struct ring_buf *output(void) { return zmk_rpc_get_rx_buf(); }
static bool can_receive(void) { return atomic_get(&studio_selected); }
static void received_bytes(void) { zmk_rpc_rx_notify(); }
static void abort_rpc_stream(void) {
    // EOF terminates any partially decoded request; the next SOF starts a new one.
    uint8_t eof = 0xAD;
    ring_buf_put(output(), &eof, 1);
    received_bytes();
    zmk_studio_core_lock();
}

static uint32_t read_input(uint8_t *data, uint32_t size) {
    return zmk_rpc_radio_tx_read(data, size);
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
    if (!zmk_2g4_is_ready() || !can_receive())
        return;
    if ((m->data[1] & SR_OPEN) && (!link.session || (int32_t)(session - link.session) > 0)) {
        sr_reset(&link, session);
        active = true;
        synchronized = true;
        abort_rpc_stream();
        // Old responses belong to the previous serial connection.
        uint8_t drop[32];
        while (read_input(drop, sizeof(drop))) {
        }
    }
    if (!active || session != link.session)
        return;
    last_rx = k_uptime_get();
    synchronized = true;
    if (m->data[1] & SR_CLOSE) {
        active = false;
        abort_rpc_stream();
        return;
    }
    sr_ack(&link, m->data);
    size_t len = m->len - SR_HEADER;
    if (can_receive() && sr_data_ready(&link, m->data, m->len) &&
        ring_buf_space_get(output()) >= len) {
        ring_buf_put(output(), m->data + SR_HEADER, len);
        link.rx++;
        received_bytes();
    }
}
static void radio_main(void) {
    for (;;) {
        if (!atomic_get(&ready)) {
            k_msleep(10);
            continue;
        }
        if (!zmk_2g4_is_ready() || !can_receive())
            active = false;
        if (active && k_uptime_get() - last_rx > 3000) {
            active = false;
            abort_rpc_stream();
        }
        // Keep RPC serialization from blocking endpoint changes after disconnect.
        if (!active && can_receive()) {
            uint8_t drop[32];
            read_input(drop, sizeof(drop));
        }
        struct radio_message message;
        while (k_msgq_get(&studio_packets, &message, K_NO_WAIT) == 0)
            process(&message);
        if (active) {
            if (synchronized && !link.pending_len) {
                link.pending_len = read_input(link.pending, SR_DATA);
            }
            emit(synchronized ? 0 : SR_OPEN);
        }
        k_msleep(active ? 5 : 20);
    }
}
K_THREAD_DEFINE(studio_radio_thread, 2048, radio_main, NULL, NULL, NULL, 5, 0, 50);
