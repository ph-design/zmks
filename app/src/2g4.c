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
#include <zmk/hid_indicators.h>
#include <zmk/2g4.h>
#include <zmk/2g4_protocol.h>
#include <zmk/2g4_crypto.h>
#include <zmk/studio/radio.h>
#include <zmk/battery.h>
#include <zmk/motion_guard.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static bool ready;
static struct k_work_delayable keepalive_work;

#define ZMK_2G4_KEEPALIVE_MS 250
#define ZMK_2G4_AUTH_RETRY_MS 20
static uint8_t consec_fail_count;

struct hid_report {
    uint8_t len;
    uint8_t data[CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH];
};

K_MSGQ_DEFINE(hid_reports, sizeof(struct hid_report), 16, 4);
static K_MUTEX_DEFINE(tx_lock);
static bool resync_reports;

static int send_payload(const uint8_t *data, size_t len, bool background) {
    if (len > CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH) {
        return -EMSGSIZE;
    }
    struct zmk_esb_payload payload = {.pipe = 0};
    memcpy(payload.data, data, len);
    int ret = zmk_2g4_crypto_encrypt(payload.data, len, sizeof(payload.data));
    if (ret < 0) {
        return ret;
    }
    payload.length = ret;
    return background ? zmk_esb_write_payload_if_idle(&payload) : zmk_esb_write_payload(&payload);
}

int zmk_2g4_send_background(const uint8_t *data, size_t len) {
    k_mutex_lock(&tx_lock, K_FOREVER);
    int ret = -ENODEV;
    if (ready) {
        ret = k_msgq_num_used_get(&hid_reports) ? -EBUSY : send_payload(data, len, true);
    }
    k_mutex_unlock(&tx_lock);
    return ret;
}

static void drain_hid_reports(void) {
    struct hid_report report;
    while (k_msgq_peek(&hid_reports, &report) == 0) {
        int ret = send_payload(report.data, report.len, false);
        if (ret == -ENOMEM) {
            break;
        }
        k_msgq_get(&hid_reports, &report, K_NO_WAIT);
        resync_reports |= ret != 0;
    }
}

static void update_indicators(uint8_t indicators) {
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    const struct zmk_endpoint_instance endpoint = {.transport = ZMK_TRANSPORT_2G4};
    if (zmk_hid_indicators_get_profile(endpoint) != indicators) {
        zmk_hid_indicators_set_profile(indicators, endpoint);
    }
#else
    ARG_UNUSED(indicators);
#endif
}

static struct k_work_delayable session_work;

static void session_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    if (!ready) {
        return;
    }
    bool was_ready = zmk_2g4_crypto_link_ready();
    struct zmk_esb_payload payload = {.pipe = 0, .noack = false};
    uint8_t type;
    int len = zmk_2g4_crypto_session_tx(payload.data, sizeof(payload.data), &type);
    if (len > 0) {
        payload.length = len;
        if (!zmk_esb_write_payload(&payload)) {
            zmk_2g4_crypto_session_sent(type);
        }
    }
    if (!was_ready && zmk_2g4_crypto_link_ready()) {
        LOG_INF("2.4G authenticated");
        zmk_2g4_send_keyboard_report();
        zmk_2g4_send_consumer_report();
    }
    if (!zmk_2g4_crypto_link_ready()) {
        uint32_t delay = consec_fail_count >= CONFIG_ZMK_2G4_MAX_CONSEC_FAILURES
                             ? ZMK_2G4_KEEPALIVE_MS
                             : ZMK_2G4_AUTH_RETRY_MS;
        k_work_reschedule(&session_work, K_MSEC(delay));
    }
}

static void keepalive_handler(struct k_work *work) {
    if (!ready) {
        return;
    }

    uint8_t data[4] = {ZMK_2G4_MSG_KEEP_ALIVE,
                       zmk_battery_is_valid() ? zmk_battery_state_of_charge() : 255};
    size_t len = 2;
    if (IS_ENABLED(CONFIG_ZMK_MOTION_GUARD)) {
        data[len++] = ZMK_2G4_KEEP_ALIVE_MOTION;
        data[len++] =
            zmk_motion_guard_locked() ? ZMK_2G4_MOTION_LOCKED : ZMK_2G4_MOTION_READY;
    }

    zmk_2g4_send_background(data, len);

    k_work_reschedule(&keepalive_work, K_MSEC(ZMK_2G4_KEEPALIVE_MS));
}

void zmk_2g4_motion_changed(void) {
    if (ready) {
        k_work_reschedule(&keepalive_work, K_NO_WAIT);
    }
}

static void esb_event_handler(const struct zmk_esb_event *event) {
    switch (event->evt_id) {
    case ZMK_ESB_EVENT_TX_SUCCESS:
        consec_fail_count = 0;
        break;
    case ZMK_ESB_EVENT_TX_FAILED:
        if (consec_fail_count < CONFIG_ZMK_2G4_MAX_CONSEC_FAILURES) {
            consec_fail_count++;
            if (consec_fail_count == CONFIG_ZMK_2G4_MAX_CONSEC_FAILURES) {
                update_indicators(0);
            }
        }
        if (!zmk_2g4_crypto_link_ready()) {
            k_work_reschedule(&session_work, K_MSEC(ZMK_2G4_KEEPALIVE_MS));
        }
        break;
    case ZMK_ESB_EVENT_RX_RECEIVED: {
        struct zmk_esb_payload rx;
        while (zmk_esb_read_rx_payload(&rx) == 0) {
            int len = zmk_2g4_crypto_decrypt(rx.data, rx.length);
            if (len == 0) {
                k_work_reschedule(&session_work, K_NO_WAIT);
            }
            if (len == 2 && rx.data[0] == ZMK_2G4_ACK_LED_INDICATORS) {
                update_indicators(rx.data[1] & 0x1f);
            } else if (len > 0) {
                studio_radio_received(rx.data, len);
            }
        }
        break;
    }
    }

    k_mutex_lock(&tx_lock, K_FOREVER);
    bool resync = false;
    if (ready) {
        resync_reports |= event->evt_id == ZMK_ESB_EVENT_TX_FAILED;
        drain_hid_reports();
        if (event->evt_id == ZMK_ESB_EVENT_TX_SUCCESS && resync_reports &&
            !k_msgq_num_used_get(&hid_reports) && zmk_2g4_crypto_link_ready()) {
            resync_reports = false;
            resync = true;
        }
    }
    k_mutex_unlock(&tx_lock);
    if (resync) {
        zmk_2g4_send_keyboard_report();
        zmk_2g4_send_consumer_report();
    }
}

static int send_report(uint8_t report_type, const uint8_t *body, size_t len) {
    if (len >= CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH) {
        return -EMSGSIZE;
    }
    struct hid_report report = {.len = (uint8_t)(len + 1), .data = {report_type}};
    memcpy(&report.data[1], body, len);

    k_mutex_lock(&tx_lock, K_FOREVER);
    int ret = -ENODEV;
    if (ready) {
        ret = k_msgq_num_used_get(&hid_reports) ? -ENOMEM
                                              : send_payload(report.data, report.len, false);
        if (ret == -ENOMEM) {
            ret = k_msgq_put(&hid_reports, &report, K_NO_WAIT);
            if (ret) {
                resync_reports = true;
                ret = -EAGAIN;
            }
        }
        if (!ret) {
            k_work_reschedule(&keepalive_work, K_MSEC(ZMK_2G4_KEEPALIVE_MS));
        }
    }
    k_mutex_unlock(&tx_lock);
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

int zmk_2g4_wait_for_tx(void) {
    int64_t deadline = k_uptime_get() + 100;
    while (ready) {
        k_mutex_lock(&tx_lock, K_FOREVER);
        drain_hid_reports();
        bool idle = !k_msgq_num_used_get(&hid_reports) && zmk_esb_is_idle();
        k_mutex_unlock(&tx_lock);
        if (idle) {
            return 0;
        }
        if (k_uptime_get() >= deadline) {
            return -ETIMEDOUT;
        }
        k_msleep(1);
    }
    return 0;
}

int zmk_2g4_start(void) {
    if (ready) {
        return 0;
    }

    int auth_err = zmk_2g4_crypto_reserve_session_id();
    if (auth_err) {
        return auth_err;
    }
    auth_err = zmk_2g4_crypto_session_start();
    if (auth_err) {
        return auth_err;
    }
    k_work_init_delayable(&session_work, session_work_handler);
    k_work_init_delayable(&keepalive_work, keepalive_handler);

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

    uint8_t base_addr[4];
    uint8_t prefix[1];
    struct zmk_2g4_addr addr;
    zmk_2g4_addr_get(&addr);
    memcpy(base_addr, addr.base, sizeof(base_addr));
    prefix[0] = addr.prefix;

    zmk_esb_set_base_address_0(base_addr);
    zmk_esb_set_prefixes(prefix, 1);

    zmk_esb_set_rf_channel(addr.rf_channel);
    zmk_esb_set_tx_power(CONFIG_ZMK_2G4_TX_POWER);

    ready = true;
    LOG_INF("2.4G transport started (%s addr, ch=%u)",
            zmk_2g4_addr_is_paired() ? "paired" : "default", addr.rf_channel);

    studio_radio_started();
    k_work_schedule(&session_work, K_NO_WAIT);
    send_boot_announcement();

    k_work_reschedule(&keepalive_work, K_MSEC(ZMK_2G4_KEEPALIVE_MS));

    return 0;
}

int zmk_2g4_stop(void) {
    if (!ready) {
        return 0;
    }

    k_mutex_lock(&tx_lock, K_FOREVER);
    ready = false;
    k_msgq_purge(&hid_reports);
    resync_reports = false;
    k_work_cancel_delayable(&session_work);
    k_work_cancel_delayable(&keepalive_work);
    zmk_esb_flush_tx();
    zmk_esb_flush_rx();
    zmk_esb_disable();
    k_mutex_unlock(&tx_lock);
    update_indicators(0);
    LOG_INF("2.4G transport stopped");
    return 0;
}
