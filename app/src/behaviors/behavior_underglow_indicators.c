/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_underglow_indicators

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zmk/hid_indicators.h>
#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/underglow_color_changed.h>
#include <zmk/rgb_underglow_layer.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct underglow_indicators_data {
    zmk_hid_indicators_t indicators;
    uint32_t layers;
};

struct underglow_indicators_config {
    int indicator;
};


struct capslock_indicator_override {
    bool has_override;
    bool enabled;
    uint32_t off_color;
    uint32_t on_color;
    uint8_t key_pos;
    uint8_t layer_id;
    bool has_pos;
};

static struct capslock_indicator_override caps_override = {
    .has_override = false,
    .enabled = true,
    .off_color = 0,
    .on_color = 0,
    .key_pos = 0,
    .layer_id = ZMK_STATUS_INDICATOR_ANY_LAYER,
    .has_pos = false,
};

static uint8_t capslock_key_position = 0;
static bool capslock_key_position_set = false;

struct connection_indicator_state {
    bool has_override;
    bool enabled;
    uint32_t usb_color;
    uint32_t bt_color;
    uint8_t key_pos;
    uint8_t layer_id;
    bool has_pos;
};

static struct connection_indicator_state conn_state = {
    .has_override = false,
    .enabled = false,
    .usb_color = 0xFFFFFF,
    .bt_color = 0x0000FF,
    .key_pos = 0,
    .layer_id = ZMK_STATUS_INDICATOR_ANY_LAYER,
    .has_pos = false,
};

static uint8_t conn_key_position = 0;
static bool conn_key_position_set = false;

static int underglow_indicators_init(const struct device *dev) { return 0; };

static int underglow_indicators_process(struct zmk_behavior_binding *binding,
                                        struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    if (dev == NULL) {
        return binding->param1;
    }
    struct underglow_indicators_data *data = dev->data;
    const struct underglow_indicators_config *config = dev->config;
    data->layers |= BIT(event.layer);

    if (!capslock_key_position_set && config->indicator == CAPS_LOCK) {
        capslock_key_position = (uint8_t)event.position;
        capslock_key_position_set = true;
    }

    if (!conn_key_position_set && config->indicator == CONNECTION) {
        conn_key_position = (uint8_t)event.position;
        conn_key_position_set = true;
    }

    if (caps_override.has_override && config->indicator == CAPS_LOCK) {
        if (!caps_override.enabled) {
            return 0;
        }
        if (caps_override.has_pos) {
            return 0;
        }
        if (data->indicators & BIT(config->indicator)) {
            return caps_override.on_color | 0x01000000;
        } else {
            return caps_override.off_color | 0x01000000;
        }
    }

    if (conn_state.has_override && config->indicator == CONNECTION) {
        if (!conn_state.enabled) {
            return 0;
        }
        if (conn_state.has_pos) {
            return 0;
        }
        struct zmk_endpoint_instance ep = zmk_endpoints_selected();
        return (ep.transport == ZMK_TRANSPORT_USB) ? (conn_state.usb_color | 0x01000000)
                                                   : (conn_state.bt_color | 0x01000000);
    }

    if (config->indicator == CONNECTION) {
        struct zmk_endpoint_instance ep = zmk_endpoints_selected();
        return (ep.transport == ZMK_TRANSPORT_USB) ? (binding->param1 | 0x01000000)
                                                   : (binding->param2 | 0x01000000);
    }

    if (data->indicators & BIT(config->indicator))
        return binding->param2;
    else
        return binding->param1;
}

static const struct behavior_driver_api underglow_indicators_driver_api = {
    .binding_pressed = underglow_indicators_process,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

static int underglow_indicators_listener(const zmk_event_t *eh);

ZMK_LISTENER(behavior_underglow_indicators, underglow_indicators_listener);
ZMK_SUBSCRIPTION(behavior_underglow_indicators, zmk_hid_indicators_changed);

static struct underglow_indicators_data underglow_indicators_data = {.indicators = 0, .layers = 0};

static int underglow_indicators_listener(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    underglow_indicators_data.indicators = ev->indicators;
    raise_zmk_underglow_color_changed((struct zmk_underglow_color_changed){
        .layers = underglow_indicators_data.layers, .wakeup = true});

    return ZMK_EV_EVENT_BUBBLE;
}

static inline void indicator_notify_changed(void) {
    raise_zmk_underglow_color_changed((struct zmk_underglow_color_changed){
        .layers = underglow_indicators_data.layers, .wakeup = true});
}

int zmk_capslock_indicator_get_state(bool *enabled, uint32_t *off_color, uint32_t *on_color,
                                     uint8_t *key_pos, uint8_t *layer_id) {
    *enabled = caps_override.has_override ? caps_override.enabled : true;
    *off_color = caps_override.has_override ? caps_override.off_color : 0;
    *on_color = caps_override.has_override ? caps_override.on_color : 0;
    *key_pos = caps_override.has_pos ? caps_override.key_pos : capslock_key_position;
    *layer_id = caps_override.layer_id;
    return 0;
}

int zmk_capslock_indicator_set_enabled(bool enabled) {
    caps_override.has_override = true;
    caps_override.enabled = enabled;
    indicator_notify_changed();
    return 0;
}

int zmk_capslock_indicator_set_off_color(uint32_t color) {
    caps_override.has_override = true;
    caps_override.off_color = color;
    indicator_notify_changed();
    return 0;
}

int zmk_capslock_indicator_set_on_color(uint32_t color) {
    caps_override.has_override = true;
    caps_override.on_color = color;
    indicator_notify_changed();
    return 0;
}

int zmk_capslock_indicator_set_key_pos(uint8_t key_pos) {
    caps_override.has_override = true;
    caps_override.has_pos = true;
    caps_override.key_pos = key_pos;
    indicator_notify_changed();
    return 0;
}

int zmk_capslock_indicator_set_layer_id(uint8_t layer_id) {
    caps_override.has_override = true;
    caps_override.layer_id = layer_id;
    indicator_notify_changed();
    return 0;
}

bool zmk_capslock_indicator_resolve(uint8_t top_layer, uint8_t *out_key_pos, uint32_t *out_color) {
    if (!caps_override.has_pos) {
        return false;
    }
    if (caps_override.has_override && !caps_override.enabled) {
        return false;
    }
    if (caps_override.layer_id != ZMK_STATUS_INDICATOR_ANY_LAYER &&
        caps_override.layer_id != top_layer) {
        return false;
    }

    bool caps_on = (zmk_hid_indicators_get_current_profile() & BIT(CAPS_LOCK)) != 0;
    *out_key_pos = caps_override.key_pos;
    *out_color = caps_on ? caps_override.on_color : caps_override.off_color;
    return true;
}

int zmk_connection_indicator_get_state(bool *enabled, uint32_t *usb_color, uint32_t *bt_color,
                                       uint8_t *key_pos, uint8_t *layer_id) {
    *enabled = conn_state.has_override ? conn_state.enabled : conn_key_position_set;
    *usb_color = conn_state.has_override ? conn_state.usb_color : 0xFFFFFF;
    *bt_color = conn_state.has_override ? conn_state.bt_color : 0x0000FF;
    *key_pos = conn_state.has_pos ? conn_state.key_pos : conn_key_position;
    *layer_id = conn_state.layer_id;
    return 0;
}

int zmk_connection_indicator_set_enabled(bool enabled) {
    conn_state.has_override = true;
    conn_state.enabled = enabled;
    indicator_notify_changed();
    return 0;
}

int zmk_connection_indicator_set_usb_color(uint32_t color) {
    conn_state.has_override = true;
    conn_state.usb_color = color;
    indicator_notify_changed();
    return 0;
}

int zmk_connection_indicator_set_bt_color(uint32_t color) {
    conn_state.has_override = true;
    conn_state.bt_color = color;
    indicator_notify_changed();
    return 0;
}

int zmk_connection_indicator_set_key_pos(uint8_t key_pos) {
    conn_state.has_override = true;
    conn_state.has_pos = true;
    conn_state.key_pos = key_pos;
    indicator_notify_changed();
    return 0;
}

int zmk_connection_indicator_set_layer_id(uint8_t layer_id) {
    conn_state.has_override = true;
    conn_state.layer_id = layer_id;
    indicator_notify_changed();
    return 0;
}

bool zmk_connection_indicator_resolve(uint8_t top_layer, uint8_t *out_key_pos,
                                      uint32_t *out_color) {
    if (!conn_state.has_pos) {
        return false;
    }
    if (!conn_state.enabled) {
        return false;
    }
    if (conn_state.layer_id != ZMK_STATUS_INDICATOR_ANY_LAYER &&
        conn_state.layer_id != top_layer) {
        return false;
    }

    struct zmk_endpoint_instance ep = zmk_endpoints_selected();
    *out_key_pos = conn_state.key_pos;
    *out_color = (ep.transport == ZMK_TRANSPORT_USB) ? conn_state.usb_color : conn_state.bt_color;
    return true;
}

static int conn_endpoint_listener(const zmk_event_t *eh) {
    if (conn_state.enabled || conn_key_position_set) {
        indicator_notify_changed();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(connection_indicator, conn_endpoint_listener);
ZMK_SUBSCRIPTION(connection_indicator, zmk_endpoint_changed);

struct capslock_settings_data {
    bool enabled;
    uint32_t off_color;
    uint32_t on_color;
    uint8_t key_pos;
    uint8_t layer_id;
    bool has_pos;
    bool has_override;
} __packed;

int zmk_capslock_indicator_save(void) {
    struct capslock_settings_data data = {
        .enabled = caps_override.enabled,
        .off_color = caps_override.off_color,
        .on_color = caps_override.on_color,
        .key_pos = caps_override.key_pos,
        .layer_id = caps_override.layer_id,
        .has_pos = caps_override.has_pos,
        .has_override = caps_override.has_override,
    };
    return settings_save_one("rgb/capslock", &data, sizeof(data));
}

static int capslock_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                 void *cb_arg) {
    if (!name || !name[0]) {
        struct capslock_settings_data data = {0};
        int rc = read_cb(cb_arg, &data, sizeof(data));
        if (rc <= 0) {
            LOG_ERR("Failed to read capslock settings (err %d)", rc);
            return rc;
        }

        bool full_struct = ((size_t)rc == sizeof(data));
        bool legacy_struct = ((size_t)rc == sizeof(data) - sizeof(bool));
        if (!full_struct && !legacy_struct) {
            LOG_WRN("Discarding capslock settings of unexpected size %d", rc);
            return 0;
        }

        caps_override.has_override = full_struct ? data.has_override : true;
        caps_override.enabled = data.enabled;
        caps_override.off_color = data.off_color;
        caps_override.on_color = data.on_color;
        caps_override.key_pos = data.key_pos;
        caps_override.layer_id = data.layer_id;
        caps_override.has_pos = data.has_pos;
        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(capslock_indicator, "rgb/capslock", NULL, capslock_settings_set,
                               NULL, NULL);

int zmk_capslock_indicator_settings_reset(void) {
    int ret = settings_delete("rgb/capslock");
    if (ret < 0 && ret != -ENOENT) {
        return ret;
    }
    caps_override.has_override = false;
    caps_override.has_pos = false;
    caps_override.layer_id = ZMK_STATUS_INDICATOR_ANY_LAYER;
    return 0;
}

struct connection_settings_data {
    bool has_override;
    bool enabled;
    uint32_t usb_color;
    uint32_t bt_color;
    uint8_t key_pos;
    uint8_t layer_id;
    bool has_pos;
} __packed;

int zmk_connection_indicator_save(void) {
    struct connection_settings_data data = {
        .has_override = conn_state.has_override,
        .enabled = conn_state.enabled,
        .usb_color = conn_state.usb_color,
        .bt_color = conn_state.bt_color,
        .key_pos = conn_state.key_pos,
        .layer_id = conn_state.layer_id,
        .has_pos = conn_state.has_pos,
    };
    return settings_save_one("rgb/conn", &data, sizeof(data));
}

static int connection_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                   void *cb_arg) {
    if (!name || !name[0]) {
        struct connection_settings_data data = {0};
        int rc = read_cb(cb_arg, &data, sizeof(data));
        if (rc <= 0) {
            LOG_ERR("Failed to read connection indicator settings (err %d)", rc);
            return rc;
        }
        if ((size_t)rc != sizeof(data)) {
            LOG_WRN("Discarding connection indicator settings of unexpected size %d", rc);
            return 0;
        }

        conn_state.has_override = data.has_override;
        conn_state.enabled = data.enabled;
        conn_state.usb_color = data.usb_color;
        conn_state.bt_color = data.bt_color;
        conn_state.key_pos = data.key_pos;
        conn_state.layer_id = data.layer_id;
        conn_state.has_pos = data.has_pos;
        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(connection_indicator, "rgb/conn", NULL, connection_settings_set,
                               NULL, NULL);

int zmk_connection_indicator_settings_reset(void) {
    int ret = settings_delete("rgb/conn");
    if (ret < 0 && ret != -ENOENT) {
        return ret;
    }
    conn_state.has_override = false;
    conn_state.has_pos = false;
    conn_state.enabled = false;
    conn_state.usb_color = 0xFFFFFF;
    conn_state.bt_color = 0x0000FF;
    conn_state.key_pos = 0;
    conn_state.layer_id = ZMK_STATUS_INDICATOR_ANY_LAYER;
    return 0;
}

#define KP_INST(n)                                                                                 \
    static struct underglow_indicators_config underglow_indicators_config_##n = {                  \
        .indicator = DT_INST_PROP(n, indicator)};                                                  \
    BEHAVIOR_DT_INST_DEFINE(n, underglow_indicators_init, NULL, &underglow_indicators_data,        \
                            &underglow_indicators_config_##n, POST_KERNEL,                         \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &underglow_indicators_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
