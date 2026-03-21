/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_underglow_indicators

// Dependencies
#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zmk/hid_indicators.h>
#include <dt-bindings/zmk/hid_indicators.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/underglow_color_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct underglow_indicators_data {
    zmk_hid_indicators_t indicators;
    uint32_t layers;
};

struct underglow_indicators_config {
    int indicator;
};

/* ------------------------------------------------------------------ */
/*  Runtime CapsLock indicator override                               */
/* ------------------------------------------------------------------ */

struct capslock_indicator_override {
    bool has_override;
    bool enabled;
    uint32_t off_color;
    uint32_t on_color;
};

static struct capslock_indicator_override caps_override = {
    .has_override = false,
    .enabled = true,
    .off_color = 0,
    .on_color = 0,
};

/* Track the key position of the first CapsLock indicator binding seen */
static uint8_t capslock_key_position = 0;
static bool capslock_key_position_set = false;

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

    /* Record the key position of this CapsLock indicator for Studio queries */
    if (!capslock_key_position_set && config->indicator == CAPS_LOCK) {
        capslock_key_position = (uint8_t)event.position;
        capslock_key_position_set = true;
    }

    if (caps_override.has_override && config->indicator == CAPS_LOCK) {
        if (!caps_override.enabled) {
            return 0; /* Disabled: transparent */
        }
        if (data->indicators & BIT(config->indicator)) {
            /* Use bit 24 as valid-color marker so 0x000000 (black) is not
             * confused with transparent (0).  The overlay only uses the
             * lower 24 bits for RGB. */
            return caps_override.on_color | 0x01000000;
        } else {
            return caps_override.off_color | 0x01000000;
        }
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

/* ------------------------------------------------------------------ */
/*  CapsLock indicator public API                                     */
/* ------------------------------------------------------------------ */

int zmk_capslock_indicator_get_state(bool *enabled, uint32_t *off_color, uint32_t *on_color,
                                     uint8_t *key_pos) {
    *enabled = caps_override.has_override ? caps_override.enabled : true;
    *off_color = caps_override.has_override ? caps_override.off_color : 0;
    *on_color = caps_override.has_override ? caps_override.on_color : 0;
    *key_pos = capslock_key_position;
    return 0;
}

int zmk_capslock_indicator_set_enabled(bool enabled) {
    caps_override.has_override = true;
    caps_override.enabled = enabled;

    raise_zmk_underglow_color_changed((struct zmk_underglow_color_changed){
        .layers = underglow_indicators_data.layers, .wakeup = true});
    return 0;
}

int zmk_capslock_indicator_set_off_color(uint32_t color) {
    caps_override.has_override = true;
    caps_override.off_color = color;

    raise_zmk_underglow_color_changed((struct zmk_underglow_color_changed){
        .layers = underglow_indicators_data.layers, .wakeup = true});
    return 0;
}

int zmk_capslock_indicator_set_on_color(uint32_t color) {
    caps_override.has_override = true;
    caps_override.on_color = color;

    raise_zmk_underglow_color_changed((struct zmk_underglow_color_changed){
        .layers = underglow_indicators_data.layers, .wakeup = true});
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Settings persistence for CapsLock indicator                       */
/* ------------------------------------------------------------------ */

struct capslock_settings_data {
    bool enabled;
    uint32_t off_color;
    uint32_t on_color;
} __packed;

int zmk_capslock_indicator_save(void) {
    struct capslock_settings_data data = {
        .enabled = caps_override.has_override ? caps_override.enabled : true,
        .off_color = caps_override.has_override ? caps_override.off_color : 0,
        .on_color = caps_override.has_override ? caps_override.on_color : 0,
    };
    return settings_save_one("rgb/capslock", &data, sizeof(data));
}

static int capslock_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                 void *cb_arg) {
    if (!name || !name[0]) {
        struct capslock_settings_data data;
        int rc = read_cb(cb_arg, &data, sizeof(data));
        if (rc <= 0) {
            LOG_ERR("Failed to read capslock settings (err %d)", rc);
            return rc;
        }

        caps_override.has_override = true;
        caps_override.enabled = data.enabled;
        caps_override.off_color = data.off_color;
        caps_override.on_color = data.on_color;
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
