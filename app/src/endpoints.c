/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4)
#include <zephyr/sys/reboot.h>
#endif

#include <stdio.h>

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/hog.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_2G4)
#include <zmk/2g4.h>
#endif
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/usb.h>
#include <zmk/usb_hid.h>
#include <zmk/event_manager.h>
#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/events/ble_active_profile_changed.h>
#endif
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/endpoint_changed.h>

#include <zephyr/logging/log.h>
#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4)
#include <zephyr/logging/log_ctrl.h>
#endif
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_BLE)
#define DEFAULT_TRANSPORT ZMK_TRANSPORT_BLE
#elif IS_ENABLED(CONFIG_ZMK_2G4)
#define DEFAULT_TRANSPORT ZMK_TRANSPORT_2G4
#else
#define DEFAULT_TRANSPORT ZMK_TRANSPORT_USB
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4)
#define DEFAULT_WIRELESS_TRANSPORT ZMK_TRANSPORT_BLE
#define RADIO_SWITCH_SETTLE_MS 50
#endif

static struct zmk_endpoint_instance current_instance = {};
#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4)
static enum zmk_transport preferred_wireless_transport = DEFAULT_WIRELESS_TRANSPORT;
static enum zmk_transport active_wireless_transport = DEFAULT_WIRELESS_TRANSPORT;
static bool radio_switching;
static bool manual_wireless_transport_selected;
static bool usb_was_ready;
#else
static enum zmk_transport preferred_transport =
    ZMK_TRANSPORT_USB; /* Used if multiple endpoints are ready */
#endif

static void update_current_endpoint(void);

#if IS_ENABLED(CONFIG_SETTINGS)
static void endpoints_save_preferred_work(struct k_work *work) {
#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4)
    settings_save_one("endpoints/preferred", &preferred_wireless_transport,
                      sizeof(preferred_wireless_transport));
#else
    settings_save_one("endpoints/preferred", &preferred_transport, sizeof(preferred_transport));
#endif
}

static struct k_work_delayable endpoints_save_work;

static int endpoints_save_preferred(void) {
    return k_work_reschedule(&endpoints_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
}
#else
static int endpoints_save_preferred(void) { return 0; }
#endif

bool zmk_endpoint_instance_eq(struct zmk_endpoint_instance a, struct zmk_endpoint_instance b) {
    if (a.transport != b.transport) {
        return false;
    }

    switch (a.transport) {
    case ZMK_TRANSPORT_USB:
        return true;

    case ZMK_TRANSPORT_BLE:
        return a.ble.profile_index == b.ble.profile_index;

#if IS_ENABLED(CONFIG_ZMK_2G4)
    case ZMK_TRANSPORT_2G4:
        return true;
#endif
    }

    LOG_ERR("Invalid transport %d", a.transport);
    return false;
}

int zmk_endpoint_instance_to_str(struct zmk_endpoint_instance endpoint, char *str, size_t len) {
    switch (endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        return snprintf(str, len, "USB");

    case ZMK_TRANSPORT_BLE:
        return snprintf(str, len, "BLE:%d", endpoint.ble.profile_index);

#if IS_ENABLED(CONFIG_ZMK_2G4)
    case ZMK_TRANSPORT_2G4:
        return snprintf(str, len, "2.4G");
#endif

    default:
        return snprintf(str, len, "Invalid");
    }
}

#define INSTANCE_INDEX_OFFSET_USB 0
#define INSTANCE_INDEX_OFFSET_BLE ZMK_ENDPOINT_USB_COUNT
#define INSTANCE_INDEX_OFFSET_2G4 (ZMK_ENDPOINT_USB_COUNT + ZMK_ENDPOINT_BLE_COUNT)

int zmk_endpoint_instance_to_index(struct zmk_endpoint_instance endpoint) {
    switch (endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        return INSTANCE_INDEX_OFFSET_USB;

    case ZMK_TRANSPORT_BLE:
        return INSTANCE_INDEX_OFFSET_BLE + endpoint.ble.profile_index;

#if IS_ENABLED(CONFIG_ZMK_2G4)
    case ZMK_TRANSPORT_2G4:
        return INSTANCE_INDEX_OFFSET_2G4;
#endif
    }

    LOG_ERR("Invalid transport %d", endpoint.transport);
    return 0;
}

#if !(IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4))
static bool is_valid_transport(enum zmk_transport transport) {
    switch (transport) {
    case ZMK_TRANSPORT_USB:
    case ZMK_TRANSPORT_BLE:
        return true;

#if IS_ENABLED(CONFIG_ZMK_2G4)
    case ZMK_TRANSPORT_2G4:
        return true;
#endif
    }

    return false;
}
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4)

#define RADIO_SWITCH_WATCHDOG_MS 1000

static void radio_switch_watchdog_expired(struct k_timer *timer) {
    if (!radio_switching) {
        return;
    }
    LOG_ERR("Radio switch watchdog expired (>%dms), rebooting", RADIO_SWITCH_WATCHDOG_MS);
    LOG_PANIC();
    sys_reboot(SYS_REBOOT_COLD);
}

static K_TIMER_DEFINE(radio_switch_watchdog, radio_switch_watchdog_expired, NULL);

static int switch_radio_transport(enum zmk_transport new_transport) {
    if (radio_switching) {
        return -EBUSY;
    }

    radio_switching = true;
    k_timer_start(&radio_switch_watchdog, K_MSEC(RADIO_SWITCH_WATCHDOG_MS), K_NO_WAIT);
    int ret = 0;

    if (new_transport == ZMK_TRANSPORT_2G4) {
        ret = zmk_ble_stop();
        if (ret) {
            LOG_ERR("BLE stop failed: %d", ret);
            goto done;
        }
        k_msleep(RADIO_SWITCH_SETTLE_MS);
        ret = zmk_2g4_start();
        if (ret) {
            LOG_ERR("2.4G start failed: %d", ret);
            goto done;
        }
    } else if (new_transport == ZMK_TRANSPORT_BLE) {
        zmk_2g4_stop();
        ret = zmk_ble_start();
        if (ret) {
            LOG_ERR("BLE start failed: %d", ret);
            goto done;
        }
    } else {
        ret = -EINVAL;
    }

done:
    if (ret == 0) {
        active_wireless_transport = new_transport;
    }

    radio_switching = false;
    k_timer_stop(&radio_switch_watchdog);
    return ret;
}

static bool is_wireless_transport(enum zmk_transport transport) {
    return transport == ZMK_TRANSPORT_BLE || transport == ZMK_TRANSPORT_2G4;
}

static int apply_wireless_transport(enum zmk_transport transport) {
    if (!is_wireless_transport(transport)) {
        return -EINVAL;
    }

    enum zmk_transport previous_transport = active_wireless_transport;
    int ret = switch_radio_transport(transport);
    if (ret) {
        if (previous_transport != transport) {
            LOG_WRN("Wireless switch failed, restoring transport %d", previous_transport);
            switch_radio_transport(previous_transport);
        }
        return ret;
    }

    preferred_wireless_transport = transport;
    update_current_endpoint();
    return 0;
}
#endif

int zmk_endpoints_select_transport(enum zmk_transport transport) {
    LOG_DBG("Selected endpoint transport %d", transport);

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4)
    if (is_wireless_transport(transport)) {
        enum zmk_transport previous_transport = preferred_wireless_transport;

        manual_wireless_transport_selected = true;
        int ret = apply_wireless_transport(transport);
        if (ret) {
            return ret;
        }

        if (previous_transport != preferred_wireless_transport) {
            endpoints_save_preferred();
        }

        return 0;
    }

    if (transport != ZMK_TRANSPORT_USB) {
        return -EINVAL;
    }

    manual_wireless_transport_selected = false;
    update_current_endpoint();
    return 0;
#else
    bool transport_changed = (preferred_transport != transport);
    preferred_transport = transport;

    if (transport_changed) {
        endpoints_save_preferred();
    }

    update_current_endpoint();

    return 0;
#endif
}

int zmk_endpoints_toggle_transport(void) {
    enum zmk_transport new_transport;

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4)
    switch (preferred_wireless_transport) {
    case ZMK_TRANSPORT_BLE:
        new_transport = ZMK_TRANSPORT_2G4;
        break;
    case ZMK_TRANSPORT_2G4:
    default:
        new_transport = ZMK_TRANSPORT_BLE;
        break;
    }
#elif IS_ENABLED(CONFIG_ZMK_2G4)
    new_transport =
        (preferred_transport == ZMK_TRANSPORT_USB) ? ZMK_TRANSPORT_2G4 : ZMK_TRANSPORT_USB;
#else
    new_transport =
        (preferred_transport == ZMK_TRANSPORT_USB) ? ZMK_TRANSPORT_BLE : ZMK_TRANSPORT_USB;
#endif
    return zmk_endpoints_select_transport(new_transport);
}

struct zmk_endpoint_instance zmk_endpoints_selected(void) { return current_instance; }

static int send_keyboard_report(void) {
    switch (current_instance.transport) {
    case ZMK_TRANSPORT_USB: {
#if IS_ENABLED(CONFIG_ZMK_USB)
        int err = zmk_usb_hid_send_keyboard_report();
        if (err) {
            LOG_ERR("FAILED TO SEND OVER USB: %d", err);
        }
        return err;
#else
        LOG_ERR("USB endpoint is not supported");
        return -ENOTSUP;
#endif /* IS_ENABLED(CONFIG_ZMK_USB) */
    }

    case ZMK_TRANSPORT_BLE: {
#if IS_ENABLED(CONFIG_ZMK_BLE)
        struct zmk_hid_keyboard_report *keyboard_report = zmk_hid_get_keyboard_report();
        int err = zmk_hog_send_keyboard_report(&keyboard_report->body);
        if (err) {
            LOG_ERR("FAILED TO SEND OVER HOG: %d", err);
        }
        return err;
#else
        LOG_ERR("BLE HOG endpoint is not supported");
        return -ENOTSUP;
#endif /* IS_ENABLED(CONFIG_ZMK_BLE) */
    }

#if IS_ENABLED(CONFIG_ZMK_2G4)
    case ZMK_TRANSPORT_2G4: {
        int err = zmk_2g4_send_keyboard_report();
        if (err) {
            LOG_ERR("FAILED TO SEND OVER 2.4G: %d", err);
        }
        return err;
    }
#endif
    }

    LOG_ERR("Unhandled endpoint transport %d", current_instance.transport);
    return -ENOTSUP;
}

static int send_consumer_report(void) {
    switch (current_instance.transport) {
    case ZMK_TRANSPORT_USB: {
#if IS_ENABLED(CONFIG_ZMK_USB)
        int err = zmk_usb_hid_send_consumer_report();
        if (err) {
            LOG_ERR("FAILED TO SEND OVER USB: %d", err);
        }
        return err;
#else
        LOG_ERR("USB endpoint is not supported");
        return -ENOTSUP;
#endif /* IS_ENABLED(CONFIG_ZMK_USB) */
    }

    case ZMK_TRANSPORT_BLE: {
#if IS_ENABLED(CONFIG_ZMK_BLE)
        struct zmk_hid_consumer_report *consumer_report = zmk_hid_get_consumer_report();
        int err = zmk_hog_send_consumer_report(&consumer_report->body);
        if (err) {
            LOG_ERR("FAILED TO SEND OVER HOG: %d", err);
        }
        return err;
#else
        LOG_ERR("BLE HOG endpoint is not supported");
        return -ENOTSUP;
#endif /* IS_ENABLED(CONFIG_ZMK_BLE) */
    }

#if IS_ENABLED(CONFIG_ZMK_2G4)
    case ZMK_TRANSPORT_2G4: {
        int err = zmk_2g4_send_consumer_report();
        if (err) {
            LOG_ERR("FAILED TO SEND OVER 2.4G: %d", err);
        }
        return err;
    }
#endif
    }

    LOG_ERR("Unhandled endpoint transport %d", current_instance.transport);
    return -ENOTSUP;
}

int zmk_endpoints_send_report(uint16_t usage_page) {

    LOG_DBG("usage page 0x%02X", usage_page);
    switch (usage_page) {
    case HID_USAGE_KEY:
        return send_keyboard_report();

    case HID_USAGE_CONSUMER:
        return send_consumer_report();
    }

    LOG_ERR("Unsupported usage page %d", usage_page);
    return -ENOTSUP;
}

#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_endpoints_send_mouse_report() {
    switch (current_instance.transport) {
    case ZMK_TRANSPORT_USB: {
#if IS_ENABLED(CONFIG_ZMK_USB)
        int err = zmk_usb_hid_send_mouse_report();
        if (err) {
            LOG_ERR("FAILED TO SEND OVER USB: %d", err);
        }
        return err;
#else
        LOG_ERR("USB endpoint is not supported");
        return -ENOTSUP;
#endif /* IS_ENABLED(CONFIG_ZMK_USB) */
    }

    case ZMK_TRANSPORT_BLE: {
#if IS_ENABLED(CONFIG_ZMK_BLE)
        struct zmk_hid_mouse_report *mouse_report = zmk_hid_get_mouse_report();
        int err = zmk_hog_send_mouse_report(&mouse_report->body);
        if (err) {
            LOG_ERR("FAILED TO SEND OVER HOG: %d", err);
        }
        return err;
#else
        LOG_ERR("BLE HOG endpoint is not supported");
        return -ENOTSUP;
#endif /* IS_ENABLED(CONFIG_ZMK_BLE) */
    }

#if IS_ENABLED(CONFIG_ZMK_2G4)
    case ZMK_TRANSPORT_2G4: {
        int err = zmk_2g4_send_mouse_report();
        if (err) {
            LOG_ERR("FAILED TO SEND OVER 2.4G: %d", err);
        }
        return err;
    }
#endif
    }

    LOG_ERR("Unhandled endpoint transport %d", current_instance.transport);
    return -ENOTSUP;
}
#endif // IS_ENABLED(CONFIG_ZMK_POINTING)

#if IS_ENABLED(CONFIG_SETTINGS)

static int endpoints_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                void *cb_arg) {
    LOG_DBG("Setting endpoint value %s", name);

    if (settings_name_steq(name, "preferred", NULL)) {
        if (len != sizeof(enum zmk_transport)) {
            LOG_ERR("Invalid endpoint size (got %d expected %d)", len, sizeof(enum zmk_transport));
            return -EINVAL;
        }

        enum zmk_transport loaded_transport;
        int err = read_cb(cb_arg, &loaded_transport, sizeof(loaded_transport));
        if (err <= 0) {
            LOG_ERR("Failed to read preferred endpoint from settings (err %d)", err);
            return err;
        }

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4)
        if (is_wireless_transport(loaded_transport)) {
            preferred_wireless_transport = loaded_transport;
        } else {
            LOG_WRN("Invalid preferred wireless transport %d, resetting", loaded_transport);
            preferred_wireless_transport = DEFAULT_WIRELESS_TRANSPORT;
        }
#else
        preferred_transport = loaded_transport;

        if (!is_valid_transport(preferred_transport)) {
            LOG_WRN("Invalid preferred transport %d, resetting", preferred_transport);
            preferred_transport = DEFAULT_TRANSPORT;
        }

        update_current_endpoint();
#endif
    }

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(endpoints, "endpoints", NULL, endpoints_handle_set, NULL, NULL);

#endif /* IS_ENABLED(CONFIG_SETTINGS) */

static bool is_usb_ready(void) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    return zmk_usb_is_hid_ready();
#else
    return false;
#endif
}

#if !(IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4))
static bool is_ble_ready(void) {
#if IS_ENABLED(CONFIG_ZMK_BLE)
    return zmk_ble_active_profile_is_connected();
#else
    return false;
#endif
}

static bool is_2g4_ready(void) {
#if IS_ENABLED(CONFIG_ZMK_2G4)
    return zmk_2g4_is_ready();
#else
    return false;
#endif
}
#endif

static enum zmk_transport get_selected_transport(void) {
    bool usb = is_usb_ready();

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4)
    if (usb && !manual_wireless_transport_selected) {
        LOG_DBG("USB ready, selecting USB");
        return ZMK_TRANSPORT_USB;
    }

    LOG_DBG("Using preferred wireless transport: %d", preferred_wireless_transport);
    return preferred_wireless_transport;
#else
    bool ble = is_ble_ready();
    bool radio_2g4 = is_2g4_ready();

    int ready_count = (usb ? 1 : 0) + (ble ? 1 : 0) + (radio_2g4 ? 1 : 0);

    if (ready_count > 1) {
        LOG_DBG("Multiple transports ready. Using preferred: %d", preferred_transport);
        return preferred_transport;
    }

    if (usb) {
        LOG_DBG("Only USB is ready.");
        return ZMK_TRANSPORT_USB;
    }

    if (ble) {
        LOG_DBG("Only BLE is ready.");
        return ZMK_TRANSPORT_BLE;
    }

#if IS_ENABLED(CONFIG_ZMK_2G4)
    if (radio_2g4) {
        LOG_DBG("Only 2.4G is ready.");
        return ZMK_TRANSPORT_2G4;
    }
#endif

    LOG_DBG("No endpoint transports are ready.");
    return DEFAULT_TRANSPORT;
#endif
}

static struct zmk_endpoint_instance get_selected_instance(void) {
    struct zmk_endpoint_instance instance = {.transport = get_selected_transport()};

    switch (instance.transport) {
#if IS_ENABLED(CONFIG_ZMK_BLE)
    case ZMK_TRANSPORT_BLE:
        instance.ble.profile_index = zmk_ble_active_profile_index();
        break;
#endif // IS_ENABLED(CONFIG_ZMK_BLE)

    default:
        // No extra data for this transport.
        break;
    }

    return instance;
}

static int zmk_endpoints_init(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&endpoints_save_work, endpoints_save_preferred_work);
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4) && !IS_ENABLED(CONFIG_SETTINGS)
    int ret = switch_radio_transport(preferred_wireless_transport);
    if (ret) {
        return ret;
    }
#endif

    current_instance = get_selected_instance();

    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4)
int zmk_endpoints_apply_preferred_transport(void) {
    if (is_usb_ready()) {
        usb_was_ready = true;
        update_current_endpoint();
        return 0;
    }

    return apply_wireless_transport(preferred_wireless_transport);
}
#endif

void zmk_endpoints_clear_current(void) {
    zmk_hid_keyboard_clear();
    zmk_hid_consumer_clear();
#if IS_ENABLED(CONFIG_ZMK_POINTING)
    zmk_hid_mouse_clear();
#endif // IS_ENABLED(CONFIG_ZMK_POINTING)

    zmk_endpoints_send_report(HID_USAGE_KEY);
    zmk_endpoints_send_report(HID_USAGE_CONSUMER);
}

static void update_current_endpoint(void) {
    struct zmk_endpoint_instance new_instance = get_selected_instance();

    if (!zmk_endpoint_instance_eq(new_instance, current_instance)) {

        current_instance = new_instance;
        zmk_endpoints_clear_current();

        char endpoint_str[ZMK_ENDPOINT_STR_LEN];
        zmk_endpoint_instance_to_str(current_instance, endpoint_str, sizeof(endpoint_str));
        LOG_INF("Endpoint changed: %s", endpoint_str);

        raise_zmk_endpoint_changed((struct zmk_endpoint_changed){.endpoint = current_instance});
    }
}

static int endpoint_listener(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_2G4)
    const struct zmk_usb_conn_state_changed *usb_event = as_zmk_usb_conn_state_changed(eh);
    if (usb_event) {
        bool usb_ready = is_usb_ready();
        if (usb_ready && !usb_was_ready) {
            manual_wireless_transport_selected = false;
        }
        usb_was_ready = usb_ready;

        if (!usb_ready && usb_event->conn_state == ZMK_USB_CONN_NONE) {
            return apply_wireless_transport(preferred_wireless_transport);
        }
    }
#endif

    update_current_endpoint();
    return 0;
}

ZMK_LISTENER(endpoint_listener, endpoint_listener);
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(endpoint_listener, zmk_usb_conn_state_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(endpoint_listener, zmk_ble_active_profile_changed);
#endif

SYS_INIT(zmk_endpoints_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
