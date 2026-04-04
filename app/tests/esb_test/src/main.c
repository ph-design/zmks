/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>

LOG_MODULE_REGISTER(esb_test, LOG_LEVEL_INF);

#include <zmk/esb.h>

#define TEST_PIPE 0
#define TEST_TX_INTERVAL_MS 500
#define TEST_PAYLOAD_LEN 8

static uint32_t tx_ok;
static uint32_t tx_fail;
static uint32_t rx_count;

static void esb_event_handler(const struct zmk_esb_event *event) {
    switch (event->evt_id) {
    case ZMK_ESB_EVENT_TX_SUCCESS:
        tx_ok++;
        LOG_INF("TX OK (attempts=%u, total=%u/%u)", event->tx_attempts, tx_ok, tx_ok + tx_fail);
        break;
    case ZMK_ESB_EVENT_TX_FAILED:
        tx_fail++;
        LOG_WRN("TX FAIL (total=%u/%u)", tx_fail, tx_ok + tx_fail);
        break;
    case ZMK_ESB_EVENT_RX_RECEIVED: {
        struct zmk_esb_payload rx;
        while (zmk_esb_read_rx_payload(&rx) == 0) {
            rx_count++;
            LOG_INF("RX #%u pipe=%u len=%u: %02x %02x %02x %02x %02x %02x %02x %02x",
                     rx_count, rx.pipe, rx.length,
                     rx.data[0], rx.data[1], rx.data[2], rx.data[3],
                     rx.data[4], rx.data[5], rx.data[6], rx.data[7]);
        }
        break;
    }
    }
}

int main(void) {
    // 等 USB CDC 就绪
    usb_enable(NULL);
    k_sleep(K_SECONDS(2));

    struct zmk_esb_config cfg = {
#if IS_ENABLED(CONFIG_ESB_TEST_ROLE_PTX)
        .mode = ZMK_ESB_MODE_PTX,
#else
        .mode = ZMK_ESB_MODE_PRX,
#endif
        .bitrate = ZMK_ESB_BITRATE_2MBPS,
        .crc = ZMK_ESB_CRC_16BIT,
        .tx_mode = ZMK_ESB_TXMODE_AUTO,
        .event_handler = esb_event_handler,
        .selective_auto_ack = false,
        .retransmit_delay = 600,
        .retransmit_count = 3,
    };

    int ret = zmk_esb_init(&cfg);
    if (ret) {
        LOG_ERR("ESB init failed: %d", ret);
        return ret;
    }

    LOG_INF("=== ESB test started: role=%s channel=%d ===",
            IS_ENABLED(CONFIG_ESB_TEST_ROLE_PTX) ? "PTX" : "PRX",
            CONFIG_ZMK_ESB_DEFAULT_RF_CHANNEL);

#if IS_ENABLED(CONFIG_ESB_TEST_ROLE_PTX)
    uint32_t seq = 0;
    struct zmk_esb_payload tx = {
        .pipe = TEST_PIPE,
        .length = TEST_PAYLOAD_LEN,
        .noack = false,
    };

    while (1) {
        // 填充测试数据: [seq(4B), ~seq(4B)]
        tx.data[0] = (seq >> 0) & 0xff;
        tx.data[1] = (seq >> 8) & 0xff;
        tx.data[2] = (seq >> 16) & 0xff;
        tx.data[3] = (seq >> 24) & 0xff;
        uint32_t inv = ~seq;
        tx.data[4] = (inv >> 0) & 0xff;
        tx.data[5] = (inv >> 8) & 0xff;
        tx.data[6] = (inv >> 16) & 0xff;
        tx.data[7] = (inv >> 24) & 0xff;

        ret = zmk_esb_write_payload(&tx);
        if (ret) {
            LOG_ERR("write_payload failed: %d", ret);
        }
        seq++;
        k_sleep(K_MSEC(TEST_TX_INTERVAL_MS));
    }
#else
    ret = zmk_esb_start_rx();
    if (ret) {
        LOG_ERR("start_rx failed: %d", ret);
        return ret;
    }
    LOG_INF("PRX listening...");

    while (1) {
        k_sleep(K_SECONDS(5));
        LOG_INF("PRX stats: rx_count=%u", rx_count);
    }
#endif

    return 0;
}
