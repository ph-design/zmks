/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include <zmk/esb.h>
#include <zmk/hid.h>
#include <zmk/2g4.h>
#include <zmk/2g4_protocol.h>
#include <zmk/2g4_crypto.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_esb_payload tx_payload;
static bool ready;
static struct k_work_delayable resend_work;

#define ZMK_2G4_MAX_CONSEC_FAILURES 10
static uint8_t consec_fail_count;

static void resend_work_handler(struct k_work *work) {
    if (!ready) {
        return;
    }

    zmk_2g4_send_keyboard_report();
    zmk_2g4_send_consumer_report();
}

static void esb_event_handler(const struct zmk_esb_event *event) {
    switch (event->evt_id) {
    case ZMK_ESB_EVENT_TX_SUCCESS:
        consec_fail_count = 0;
        break;
    case ZMK_ESB_EVENT_TX_FAILED:
        zmk_esb_flush_tx();
        if (++consec_fail_count < ZMK_2G4_MAX_CONSEC_FAILURES) {
            k_work_reschedule(&resend_work, K_MSEC(1));
        } else {
            LOG_WRN("2.4G: %d consecutive TX failures, dongle unreachable", consec_fail_count);
        }
        break;
    case ZMK_ESB_EVENT_RX_RECEIVED: {
        struct zmk_esb_payload rx;
        while (zmk_esb_read_rx_payload(&rx) == 0) {
            if (rx.length < 1) {
                continue;
            }
            switch (rx.data[0]) {
            case ZMK_2G4_ACK_LED_INDICATORS:
                break;
            case ZMK_2G4_ACK_HOST_CONNECTED:
                break;
            default:
                break;
            }
        }
        break;
    }
    }
}

static int send_report(uint8_t report_type, const uint8_t *body, size_t len) {
    if (!ready) {
        return -ENODEV;
    }

    consec_fail_count = 0;

    tx_payload.pipe = 0;
    tx_payload.noack = false;
    tx_payload.length = len + 1;
    tx_payload.data[0] = report_type;
    memcpy(&tx_payload.data[1], body, MIN(len, CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH - 1));

    int enc_len =
        zmk_2g4_crypto_encrypt(tx_payload.data, len + 1, CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH);
    if (enc_len < 0) {
        return enc_len;
    }
    tx_payload.length = enc_len;

    int ret = zmk_esb_write_payload(&tx_payload);
    if (ret == -ENOMEM) {
        zmk_esb_flush_tx();
        ret = zmk_esb_write_payload(&tx_payload);
    }
    return ret;
}

int zmk_2g4_send_keyboard_report(void) {
    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    return send_report(ZMK_2G4_MSG_KEYBOARD_REPORT, (const uint8_t *)&report->body,
                       sizeof(report->body));
}

int zmk_2g4_send_consumer_report(void) {
    struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
    return send_report(ZMK_2G4_MSG_CONSUMER_REPORT, (const uint8_t *)&report->body,
                       sizeof(report->body));
}

#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_2g4_send_mouse_report(void) {
    struct zmk_hid_mouse_report *report = zmk_hid_get_mouse_report();
    return send_report(ZMK_2G4_MSG_MOUSE_REPORT, (const uint8_t *)&report->body,
                       sizeof(report->body));
}
#endif

static int send_boot_announcement(void) {
    uint32_t reset_cause = 0;
    int hr = hwinfo_get_reset_cause(&reset_cause);
    if (hr == 0) {
        // Clear so a subsequent boot reads a fresh cause
        (void)hwinfo_clear_reset_cause();
    } else {
        LOG_WRN("hwinfo_get_reset_cause failed: %d", hr);
        reset_cause = 0xFFFFFFFFu;
    }

    // Body: reset_cause
    uint8_t body[8];
    sys_put_le32(reset_cause, body);
    sys_put_le32(zmk_2g4_crypto_tx_session_id(), body + 4);

    int ret = send_report(ZMK_2G4_MSG_BOOT, body, sizeof(body));
    LOG_INF("2.4G BOOT sent: reason=0x%08x session=0x%08x ret=%d", reset_cause,
            zmk_2g4_crypto_tx_session_id(), ret);
    return ret;
}

bool zmk_2g4_is_ready(void) { return ready; }

int zmk_2g4_start(void) {
    if (ready) {
        return 0;
    }

    k_work_init_delayable(&resend_work, resend_work_handler);

    struct zmk_esb_config config = {
        .mode = ZMK_ESB_MODE_PTX,
        .bitrate = ZMK_ESB_BITRATE_2MBPS,
        .crc = ZMK_ESB_CRC_16BIT,
        .tx_mode = ZMK_ESB_TXMODE_AUTO,
        .event_handler = esb_event_handler,
        .selective_auto_ack = false,
        .retransmit_delay = CONFIG_ZMK_2G4_RETRANSMIT_DELAY,
        .retransmit_count = CONFIG_ZMK_2G4_RETRANSMIT_COUNT,
    };

    int ret = zmk_esb_init(&config);
    if (ret) {
        LOG_ERR("ESB init failed: %d", ret);
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
    zmk_esb_set_tx_power(CONFIG_ZMK_2G4_TX_POWER);

    ready = true;
    LOG_INF("2.4G transport started");

    send_boot_announcement();
    return 0;
}

int zmk_2g4_stop(void) {
    if (!ready) {
        return 0;
    }

    ready = false;
    k_work_cancel_delayable(&resend_work);
    zmk_esb_flush_tx();
    zmk_esb_flush_rx();
    zmk_esb_disable();
    LOG_INF("2.4G transport stopped");
    return 0;
}
