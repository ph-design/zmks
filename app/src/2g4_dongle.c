/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <string.h>

#include <zmk/esb.h>
#include <zmk/hid.h>
#include <zmk/usb_hid.h>
#include <zmk/2g4_protocol.h>

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators_types.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct k_work rx_work;
static struct k_work_delayable release_work;

static void release_work_handler(struct k_work *work) {
    struct zmk_hid_keyboard_report *kb = zmk_hid_get_keyboard_report();
    memset(&kb->body, 0, sizeof(kb->body));
    zmk_usb_hid_send_keyboard_report();

    struct zmk_hid_consumer_report *cs = zmk_hid_get_consumer_report();
    memset(&cs->body, 0, sizeof(cs->body));
    zmk_usb_hid_send_consumer_report();

    LOG_WRN("2G4 dongle: no data, releasing all keys");
}

static void process_rx_payload(const struct zmk_esb_payload *rx) {
    if (rx->length < 2) {
        return;
    }

    switch (rx->data[0]) {
    case ZMK_2G4_MSG_KEYBOARD_REPORT: {
        struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
        size_t body_len = MIN(rx->length - 1, sizeof(report->body));
        memcpy(&report->body, &rx->data[1], body_len);
        int err = zmk_usb_hid_send_keyboard_report();
        if (err) {
            LOG_WRN("USB keyboard report send failed: %d", err);
        }
        break;
    }
    case ZMK_2G4_MSG_CONSUMER_REPORT: {
        struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
        size_t body_len = MIN(rx->length - 1, sizeof(report->body));
        memcpy(&report->body, &rx->data[1], body_len);
        int err = zmk_usb_hid_send_consumer_report();
        if (err) {
            LOG_WRN("USB consumer report send failed: %d", err);
        }
        break;
    }
#if IS_ENABLED(CONFIG_ZMK_POINTING)
    case ZMK_2G4_MSG_MOUSE_REPORT: {
        struct zmk_hid_mouse_report *report = zmk_hid_get_mouse_report();
        size_t body_len = MIN(rx->length - 1, sizeof(report->body));
        memcpy(&report->body, &rx->data[1], body_len);
        int err = zmk_usb_hid_send_mouse_report();
        if (err) {
            LOG_WRN("USB mouse report send failed: %d", err);
        }
        break;
    }
#endif
    case ZMK_2G4_MSG_KEEP_ALIVE:
        break;
    default:
        LOG_DBG("Unknown 2G4 message type: 0x%02x", rx->data[0]);
        break;
    }

    k_work_reschedule(&release_work, K_MSEC(CONFIG_ZMK_2G4_DONGLE_RELEASE_TIMEOUT_MS));
}

static void rx_work_handler(struct k_work *work) {
    struct zmk_esb_payload rx;
    while (zmk_esb_read_rx_payload(&rx) == 0) {
        process_rx_payload(&rx);
    }
}

static void esb_event_handler(const struct zmk_esb_event *event) {
    switch (event->evt_id) {
    case ZMK_ESB_EVENT_RX_RECEIVED:
        k_work_submit(&rx_work);
        break;
    case ZMK_ESB_EVENT_TX_SUCCESS:
        break;
    case ZMK_ESB_EVENT_TX_FAILED:
        LOG_WRN("ACK payload TX failed");
        break;
    }
}

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

static int dongle_indicator_listener(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct zmk_esb_payload ack = {
        .pipe = 0,
        .noack = false,
        .length = 2,
    };
    ack.data[0] = ZMK_2G4_ACK_LED_INDICATORS;
    ack.data[1] = (uint8_t)ev->indicators;

    int ret = zmk_esb_write_payload(&ack);
    if (ret) {
        LOG_WRN("LED indicator ACK write failed: %d", ret);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_2g4_dongle, dongle_indicator_listener);
ZMK_SUBSCRIPTION(zmk_2g4_dongle, zmk_hid_indicators_changed);

#endif /* CONFIG_ZMK_HID_INDICATORS */

static int zmk_2g4_dongle_init(void) {
    k_work_init(&rx_work, rx_work_handler);
    k_work_init_delayable(&release_work, release_work_handler);

    struct zmk_esb_config config = {
        .mode = ZMK_ESB_MODE_PRX,
        .bitrate = ZMK_ESB_BITRATE_2MBPS,
        .crc = ZMK_ESB_CRC_16BIT,
        .tx_mode = ZMK_ESB_TXMODE_AUTO,
        .event_handler = esb_event_handler,
        .selective_auto_ack = false,
    };

    int ret = zmk_esb_init(&config);
    if (ret) {
        LOG_ERR("ESB PRX init failed: %d", ret);
        return ret;
    }

    uint8_t base_addr[] = {
        CONFIG_ZMK_2G4_ADDR_BASE_0,
        CONFIG_ZMK_2G4_ADDR_BASE_1,
        CONFIG_ZMK_2G4_ADDR_BASE_2,
        CONFIG_ZMK_2G4_ADDR_BASE_3,
    };
    zmk_esb_set_base_address_0(base_addr);

    uint8_t prefix[] = {CONFIG_ZMK_2G4_ADDR_PREFIX};
    zmk_esb_set_prefixes(prefix, 1);

    zmk_esb_set_rf_channel(CONFIG_ZMK_2G4_RF_CHANNEL);

    ret = zmk_esb_start_rx();
    if (ret) {
        LOG_ERR("ESB start RX failed: %d", ret);
        return ret;
    }

    LOG_INF("2.4G dongle started (ch=%d)", CONFIG_ZMK_2G4_RF_CHANNEL);
    return 0;
}

SYS_INIT(zmk_2g4_dongle_init, APPLICATION, CONFIG_ZMK_2G4_DONGLE_INIT_PRIORITY);
