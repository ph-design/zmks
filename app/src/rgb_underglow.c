/*
 * Copyright (c) 2020 The ZMK Contributors
 * Copyright (c) 2024 Kuba Birecki (zmk-rgb-fx effects ported)
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#include <zephyr/logging/log.h>

#include <zephyr/drivers/led_strip.h>
#include <drivers/ext_power.h>
#include <drivers/behavior.h>

#include <zmk/rgb_underglow.h>
#include <zmk/rgb_underglow_layer.h>

#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/matrix.h>
#include <zmk/hid_indicators.h>
#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/underglow_color_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>

#include <zmk/workqueue.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_underglow)
#error "A zmk,underglow chosen node must be declared"
#endif

#define STRIP_CHOSEN DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_underglow_layer) && IS_ENABLED(CONFIG_EXPERIMENTAL_RGB_LAYER)
#define UNDERGLOW_LAYER_ENABLED 1
#endif

static inline int effect_pixel_lookup(int led_idx) {
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    return rgb_pixel_lookup(led_idx);
#else
    return led_idx;
#endif
}

#define HUE_MAX 360
#define SAT_MAX 100
#define BRT_MAX 100

/* Animation FPS */
#define ANIMATION_FPS 60

BUILD_ASSERT(CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN <= CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX,
             "ERROR: RGB underglow maximum brightness is less than minimum brightness");

/* ========================================================================= */
/*  Color Utilities (ported from zmk-rgb-fx/color.c)                         */
/* ========================================================================= */

struct color_rgb_float {
    float r;
    float g;
    float b;
};

static void hsv_to_rgb_float(uint16_t h, uint8_t s, uint8_t v, struct color_rgb_float *rgb) {
    float sf = (float)s / 100.0f;
    float vf = (float)v / 100.0f;
    float c = vf * sf;
    float a = (float)h / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(a, 2.0f) - 1.0f));
    float m = vf - c;

    float r, g, b;
    switch ((uint8_t)a % 6) {
    case 0: r = c; g = x; b = 0; break;
    case 1: r = x; g = c; b = 0; break;
    case 2: r = 0; g = c; b = x; break;
    case 3: r = 0; g = x; b = c; break;
    case 4: r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
    }
    rgb->r = r + m;
    rgb->g = g + m;
    rgb->b = b + m;
}

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_GAMMA_CORRECTION)
static const uint8_t gamma_lut[256] = {
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,
      1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,
      3,   3,   3,   3,   3,   4,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,
      6,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,  11,  11,  11,  12,
     12,  13,  13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  18,  18,  19,  19,
     20,  20,  21,  22,  22,  23,  23,  24,  25,  25,  26,  26,  27,  28,  28,  29,
     30,  30,  31,  32,  33,  33,  34,  35,  35,  36,  37,  38,  39,  39,  40,  41,
     42,  43,  43,  44,  45,  46,  47,  48,  49,  49,  50,  51,  52,  53,  54,  55,
     56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,
     73,  74,  75,  76,  77,  78,  79,  81,  82,  83,  84,  85,  87,  88,  89,  90,
     91,  93,  94,  95,  97,  98,  99, 100, 102, 103, 105, 106, 107, 109, 110, 111,
    113, 114, 116, 117, 119, 120, 121, 123, 124, 126, 127, 129, 130, 132, 133, 135,
    137, 138, 140, 141, 143, 145, 146, 148, 149, 151, 153, 154, 156, 158, 159, 161,
    163, 165, 166, 168, 170, 172, 173, 175, 177, 179, 181, 182, 184, 186, 188, 190,
    192, 194, 196, 197, 199, 201, 203, 205, 207, 209, 211, 213, 215, 217, 219, 221,
    223, 225, 227, 229, 231, 234, 236, 238, 240, 242, 244, 246, 248, 251, 253, 255,
};
#endif

static float dither_err[STRIP_NUM_PIXELS][3];

static void dither_reset(void) {
    memset(dither_err, 0, sizeof(dither_err));
}

static inline uint8_t dither_led_channel(float v, float *err) {
    float adjusted = v + *err;
    if (adjusted < 0.0f)
        adjusted = 0.0f;
    if (adjusted > 1.0f)
        adjusted = 1.0f;
    uint8_t x = (uint8_t)(adjusted * 255.0f + 0.5f);
    /* Store the signed quantisation error (in 0–1 space) for next frame */
    *err = adjusted - (float)x / 255.0f;
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_GAMMA_CORRECTION)
    return gamma_lut[x];
#else
    return x;
#endif
}

static void rgb_float_to_led(int idx, const struct color_rgb_float *rgb, struct led_rgb *led) {
    float r = rgb->r > 1.0f ? 1.0f : (rgb->r < 0 ? 0 : rgb->r);
    float g = rgb->g > 1.0f ? 1.0f : (rgb->g < 0 ? 0 : rgb->g);
    float b = rgb->b > 1.0f ? 1.0f : (rgb->b < 0 ? 0 : rgb->b);
    led->r = dither_led_channel(r, &dither_err[idx][0]);
    led->g = dither_led_channel(g, &dither_err[idx][1]);
    led->b = dither_led_channel(b, &dither_err[idx][2]);
}

static void interpolate_rgb(const struct color_rgb_float *from, const struct color_rgb_float *to,
                            struct color_rgb_float *result, float step) {
    result->r = from->r + (to->r - from->r) * step;
    result->g = from->g + (to->g - from->g) * step;
    result->b = from->b + (to->b - from->b) * step;
}

// brightness scaling

static struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb) {
    hsb.b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN +
            (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX - CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN) * hsb.b / BRT_MAX;
    return hsb;
}

/* ========================================================================= */
/*  Effect Enumeration & State                                               */
/* ========================================================================= */

enum rgb_underglow_effect {
    UNDERGLOW_EFFECT_SOLID,        /*  0  Static / solid */
    UNDERGLOW_EFFECT_BREATHING,    /*  1  Breathing (parabolic pulse) */
    UNDERGLOW_EFFECT_SPECTRUM,     /*  2  All LEDs same hue, cycling */
    UNDERGLOW_EFFECT_RAINBOW,      /*  3  Hue gradient across strip */
    UNDERGLOW_EFFECT_GRADIENT,     /*  4  Dual-colour linear gradient */
    UNDERGLOW_EFFECT_WAVE,         /*  5  Wave */
    UNDERGLOW_EFFECT_KNIGHT,       /*  6  Knight rider */
    UNDERGLOW_EFFECT_TWINKLE,      /*  7  Twinkle / glitter */
    UNDERGLOW_EFFECT_SPARKLE,      /*  8  Sparkle */
    UNDERGLOW_EFFECT_RAINDROPS,    /*  9  Raindrops */
    UNDERGLOW_EFFECT_ALPHAS_MODS,  /* 10  Dual-hue alpha/modifier split */
    UNDERGLOW_EFFECT_REACTIVE,        /* 11  Reactive keypress (enhanced) */
    UNDERGLOW_EFFECT_RIPPLE,          /* 12  Keypress ripple */
    UNDERGLOW_EFFECT_REACTIVE_WIDE,   /* 13  Wide radial reactive pulse */
    UNDERGLOW_EFFECT_REACTIVE_NEXUS,  /* 14  Cross/nexus reactive pulse */
    UNDERGLOW_EFFECT_TYPING_HEATMAP,  /* 15  Typing heatmap */
    
    UNDERGLOW_EFFECT_NUMBER
};

struct rgb_underglow_state {
    struct zmk_led_hsb color;
    uint8_t animation_speed;
    uint8_t current_effect;
    uint16_t animation_step;
    bool on;
    bool layer_enabled;
    uint8_t _reserved; /* padding to invalidate 0.3 saved settings on OTA */
};

static const struct device *led_strip;

static struct led_rgb pixels[STRIP_NUM_PIXELS];
/* Float pixel buffer for new effects rendering */
static struct color_rgb_float fx_pixels[STRIP_NUM_PIXELS];

static struct rgb_underglow_state state;

/* Convenience: convert user's current color (at full brightness) to RGB */
static void user_color_rgb_float(struct color_rgb_float *rgb) {
    hsv_to_rgb_float(state.color.h, state.color.s, 100, rgb);
}

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

/* Get the brightness factor from user state (0.0 ~ 1.0) */
static float get_brightness_factor(void) {
    return (float)hsb_scale_min_max(state.color).b / (float)BRT_MAX;
}

/* Wrap a hue value into the [0, 360) range */
static inline float hue_wrap(float h) {
    while (h >= 360.0f)
        h -= 360.0f;
    while (h < 0.0f)
        h += 360.0f;
    return h;
}

/* ========================================================================= */
/*  Per-LED Physical Coordinates (normalised 0-255, indexed by strip pos)    */
/* ========================================================================= */

/* Inverse pixel lookup: key matrix position → LED strip index.
 * Built at init from the pixel-lookup / transform DT property. */
static uint8_t key_to_pixel[STRIP_NUM_PIXELS];
static uint8_t led_pos_x[STRIP_NUM_PIXELS];
static uint8_t led_pos_y[STRIP_NUM_PIXELS];
static bool positions_available = false;

/* Normalised position helpers — effects use these for spatial calculations.
 * When led-positions are available, returns physical coordinate (0.0-1.0).
 * Otherwise falls back to index-based position (1D strip order).
 */
static inline float led_norm_x(int i) {
    if (positions_available)
        return (float)led_pos_x[i] / 255.0f;
    return (float)effect_pixel_lookup(i) / (float)(STRIP_NUM_PIXELS > 1 ? STRIP_NUM_PIXELS - 1 : 1);
}

static inline float led_norm_y(int i) {
    if (positions_available)
        return (float)led_pos_y[i] / 255.0f;
    return 0.5f; /* 1D fallback: all LEDs on a single horizontal line */
}

/* Get normalised source coordinates for a key matrix position (for keypress events) */
static inline float key_src_x(uint32_t position) {
    if (position < STRIP_NUM_PIXELS)
        return led_norm_x(key_to_pixel[position]);
    return 0.5f;
}

static inline float key_src_y(uint32_t position) {
    if (position < STRIP_NUM_PIXELS)
        return led_norm_y(key_to_pixel[position]);
    return 0.5f;
}

/* ========================================================================= */
/*  Sparkle Effect State (ported from zmk-rgb-fx/sparkle.c)                  */
/* ========================================================================= */

struct sparkle_pixel {
    struct color_rgb_float color;
    uint16_t total_frames;
    uint16_t counter;
    float step;
};

static struct sparkle_pixel sparkle_data[STRIP_NUM_PIXELS];

static void sparkle_generate_pixel(int idx, bool offset_counter) {
    user_color_rgb_float(&sparkle_data[idx].color);
    sparkle_data[idx].total_frames = (3 * ANIMATION_FPS) / ((rand() % 16) + 1);
    if (sparkle_data[idx].total_frames < 2)
        sparkle_data[idx].total_frames = 2;
    sparkle_data[idx].counter = 2 * sparkle_data[idx].total_frames;
    sparkle_data[idx].step = 1.0f / (float)sparkle_data[idx].total_frames;
    if (offset_counter) {
        sparkle_data[idx].counter = rand() % sparkle_data[idx].counter;
        if (sparkle_data[idx].counter < 1)
            sparkle_data[idx].counter = 1;
    }
}

/* ========================================================================= */
/*  Keypress Event Infrastructure (shared by key-interactive effects)        */
/* ========================================================================= */

#define EFFECT_EVENT_MSGQ_SIZE 32

struct effect_event {
    uint32_t position;
};
K_MSGQ_DEFINE(effect_event_msgq, sizeof(struct effect_event), EFFECT_EVENT_MSGQ_SIZE, 4);

/* ========================================================================= */
/*  Ripple Effect State                                                      */
/* ========================================================================= */

#define RIPPLE_MAX_EVENTS 16
#define RIPPLE_WIDTH 40

struct ripple_event {
    uint32_t pixel_id;
    uint16_t distance;
    uint8_t counter;
};

static struct ripple_event ripple_events[RIPPLE_MAX_EVENTS];
static uint8_t ripple_events_start = 0;
static uint8_t ripple_num_events = 0;
static struct k_mutex ripple_mutex;

/* ========================================================================= */
/*  Reactive Effect State                                                    */
/* ========================================================================= */

/* Per-pixel fade brightness: 255 = just pressed, 0 = off */
static uint8_t reactive_brightness[STRIP_NUM_PIXELS];

/* ========================================================================= */
/*  Gradient & Solid Effect State                                            */
/* ========================================================================= */

static float gradient_offset = 0.0f;

/* ========================================================================= */
/*  Non-linear Speed Curve                                                   */
/* ========================================================================= */

/*
 * Perceptually-uniform speed curve based on Weber-Fechner law.
 *
 * Formula:  effective(n) = Smin * (Smax/Smin) ^ ((n-1)/(N-1))
 *           Smin = 1.0,  Smax = 7.0,  N = 10
 *
 * Each step is ~24% faster than the previous, giving equal perceived
 * speed increments across all 10 levels.
 *
 *   User:      1     2     3     4     5     6     7     8     9    10
 *   Effective: 1.00  1.24  1.54  1.91  2.38  2.95  3.66  4.54  5.64  7.00
 */
static const float speed_curve[11] = {
    0.0f, 1.00f, 1.24f, 1.54f, 1.91f, 2.38f, 2.95f, 3.66f, 4.54f, 5.64f, 7.00f,
};

static inline float anim_speed(void) {
    uint8_t s = state.animation_speed;
    return speed_curve[s <= 10 ? s : 10];
}

/* ========================================================================= */
/*  Effect Rendering Functions                                               */
/* ========================================================================= */

/* Each effect file can define:
 *   - render function    (required)
 *   - on_keypress handler (optional, for key-interactive effects)
 *   - reset function      (optional, to clear effect state)
 */
#include "rgb_effects/effect_solid.inc.c"
#include "rgb_effects/effect_breathing.inc.c"
#include "rgb_effects/effect_spectrum.inc.c"
#include "rgb_effects/effect_rainbow.inc.c"
#include "rgb_effects/effect_gradient.inc.c"
#include "rgb_effects/effect_wave.inc.c"
#include "rgb_effects/effect_knight.inc.c"
#include "rgb_effects/effect_twinkle.inc.c"
#include "rgb_effects/effect_sparkle.inc.c"
#include "rgb_effects/effect_raindrops.inc.c"
#include "rgb_effects/effect_alphas_mods.inc.c"
#include "rgb_effects/effect_reactive_enhanced.inc.c"
#include "rgb_effects/effect_ripple.inc.c"
#include "rgb_effects/effect_reactive_wide.inc.c"
#include "rgb_effects/effect_reactive_nexus.inc.c"
#include "rgb_effects/effect_typing_heatmap.inc.c"
/* Note: original reactive implementation is no longer included —
    REACTIVE is mapped to the enhanced implementation above. */

/* ========================================================================= */
/*  Effect Descriptor Table                                                  */
/* ========================================================================= */

/* To add a new key-interactive effect:
 * 1. Add entry to enum rgb_underglow_effect
 * 2. Create rgb_effects/effect_xxx.inc.c with render / on_keypress / reset
 * 3. Add a row to effect_table[] below
 * That's it — tick, event listener, init, and reset are all table-driven.
 */

typedef void (*effect_render_fn)(void);
typedef void (*effect_keypress_fn)(uint32_t position);
typedef void (*effect_reset_fn)(void);

struct rgb_effect_desc {
    effect_render_fn render;
    effect_keypress_fn on_keypress; /* NULL = effect ignores key events */
    effect_reset_fn reset;          /* NULL = no state to reset */
};

static const struct rgb_effect_desc effect_table[UNDERGLOW_EFFECT_NUMBER] = {
    [UNDERGLOW_EFFECT_SOLID] = {.render = zmk_rgb_underglow_effect_solid},
    [UNDERGLOW_EFFECT_BREATHING] = {.render = zmk_rgb_underglow_effect_breathing,
                                    .reset = breathing_reset},
    [UNDERGLOW_EFFECT_SPECTRUM] = {.render = zmk_rgb_underglow_effect_spectrum,
                                   .reset = spectrum_reset},
    [UNDERGLOW_EFFECT_RAINBOW] = {.render = zmk_rgb_underglow_effect_rainbow,
                                  .reset = rainbow_reset},
    [UNDERGLOW_EFFECT_GRADIENT] = {.render = zmk_rgb_underglow_effect_gradient,
                                   .reset = gradient_reset},
    [UNDERGLOW_EFFECT_WAVE] = {.render = zmk_rgb_underglow_effect_wave, .reset = wave_reset},
    [UNDERGLOW_EFFECT_KNIGHT] = {.render = zmk_rgb_underglow_effect_knight, .reset = knight_reset},
    [UNDERGLOW_EFFECT_TWINKLE] = {.render = zmk_rgb_underglow_effect_twinkle,
                                  .reset = twinkle_reset},
    [UNDERGLOW_EFFECT_SPARKLE] = {.render = zmk_rgb_underglow_effect_sparkle,
                                  .reset = sparkle_init_all},
    [UNDERGLOW_EFFECT_RAINDROPS] = {.render = zmk_rgb_underglow_effect_raindrops,
                                    .reset = raindrops_reset},
    [UNDERGLOW_EFFECT_ALPHAS_MODS] = {.render = zmk_rgb_underglow_effect_alphas_mods,
                                      .reset = alphas_mods_reset},
    [UNDERGLOW_EFFECT_REACTIVE] = {.render = zmk_rgb_underglow_effect_reactive_enhanced,
                                   .on_keypress = reactive_add_event,
                                   .reset = reactive_enhanced_reset},
    [UNDERGLOW_EFFECT_RIPPLE] = {.render = zmk_rgb_underglow_effect_ripple,
                                 .on_keypress = ripple_add_event,
                                 .reset = ripple_reset},
    [UNDERGLOW_EFFECT_REACTIVE_WIDE] = {.render = zmk_rgb_underglow_effect_reactive_wide,
                                        .on_keypress = reactive_wide_add_event,
                                        .reset = reactive_wide_reset},
    [UNDERGLOW_EFFECT_REACTIVE_NEXUS] = {.render = zmk_rgb_underglow_effect_reactive_nexus,
                                         .on_keypress = reactive_nexus_add_event,
                                         .reset = reactive_nexus_reset},
    [UNDERGLOW_EFFECT_TYPING_HEATMAP] = {.render = zmk_rgb_underglow_effect_typing_heatmap,
                                         .on_keypress = heatmap_add_event,
                                         .reset = heatmap_reset},
};

/* ========================================================================= */
/*  Layer Indicator Effect (kept from original)                              */
/* ========================================================================= */

/* ========================================================================= */
/*  Layer Overlay (applied on top of every effect)                           */
/* ========================================================================= */

static struct led_rgb hex_to_rgb_overlay(uint8_t r, uint8_t g, uint8_t b) {
    struct zmk_led_hsb hsb = hsb_scale_min_max(state.color);
    uint8_t lr = (uint8_t)((hsb.b * r) / 0xff);
    uint8_t lg = (uint8_t)((hsb.b * g) / 0xff);
    uint8_t lb = (uint8_t)((hsb.b * b) / 0xff);
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_GAMMA_CORRECTION)
    lr = gamma_lut[lr];
    lg = gamma_lut[lg];
    lb = gamma_lut[lb];
#endif
    return (struct led_rgb){r : lr, g : lg, b : lb};
}

static int find_led_for_key_pos(uint8_t key_pos) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        if ((uint8_t)rgb_pixel_lookup(i) == key_pos) {
            return i;
        }
    }
    return -1;
}

static void zmk_rgb_underglow_apply_status_overlay(uint8_t top_layer) {
    uint8_t key_pos;
    uint32_t color;

    if (zmk_capslock_indicator_resolve(top_layer, &key_pos, &color) && color > 0) {
        int led_idx = find_led_for_key_pos(key_pos);
        if (led_idx >= 0) {
            pixels[led_idx] = hex_to_rgb_overlay((color & 0xFF0000) >> 16,
                                                 (color & 0xFF00) >> 8, color & 0xFF);
        }
    }

    if (zmk_connection_indicator_resolve(top_layer, &key_pos, &color) && color > 0) {
        int led_idx = find_led_for_key_pos(key_pos);
        if (led_idx >= 0) {
            pixels[led_idx] = hex_to_rgb_overlay((color & 0xFF0000) >> 16,
                                                 (color & 0xFF00) >> 8, color & 0xFF);
        }
    }
}

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
static void zmk_rgb_underglow_apply_layer_overlay(void) {
#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)
    if (!zmk_rgb_layer_is_enabled()) {
        return;
    }
#endif
    uint8_t layer = rgb_underglow_top_layer();
    const struct zmk_behavior_binding *rgbmap = rgb_underglow_get_bindings(layer);
    if (rgbmap == NULL)
        return;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t midx = rgb_pixel_lookup(i);
        if (midx >= ZMK_KEYMAP_LEN)
            continue;

        const struct device *dev = zmk_behavior_get_binding(rgbmap[midx].behavior_dev);
        if (dev == NULL)
            continue;

        const struct behavior_driver_api *api = (const struct behavior_driver_api *)dev->api;
        if (api->binding_pressed == NULL)
            continue;

        struct zmk_behavior_binding_event event = {
            .position = midx, .layer = layer, .timestamp = k_uptime_get()};

        int color = api->binding_pressed((struct zmk_behavior_binding *)&rgbmap[midx], event);

        if (color > 0) {
            /* Non-zero color overrides the effect pixel */
            pixels[i] =
                hex_to_rgb_overlay((color & 0xFF0000) >> 16, (color & 0xFF00) >> 8, color & 0xFF);
        }
        /* color == 0 (___) means transparent: keep the underlying effect pixel */
    }
}
#endif

/* ========================================================================= */
/*  Tick / Timer                                                             */
/* ========================================================================= */

static void zmk_rgb_underglow_tick(struct k_work *work) {
    const struct rgb_effect_desc *eff = &effect_table[state.current_effect];

    /* Drain any pending keypress events */
    struct effect_event ev;
    while (k_msgq_get(&effect_event_msgq, &ev, K_NO_WAIT) == 0) {
        if (eff->on_keypress) {
            eff->on_keypress(ev.position);
        }
    }

    /* Run current effect renderer */
    eff->render();

    /* Convert float pixels to LED strip format */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        rgb_float_to_led(i, &fx_pixels[i], &pixels[i]);
    }

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    zmk_rgb_underglow_apply_layer_overlay();
#endif
    zmk_rgb_underglow_apply_status_overlay(rgb_underglow_top_layer());

    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }
}

K_WORK_DEFINE(underglow_tick_work, zmk_rgb_underglow_tick);

static void zmk_rgb_underglow_tick_handler(struct k_timer *timer) {
    if (!state.on) {
        return;
    }
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_tick_work);
}

K_TIMER_DEFINE(underglow_tick, zmk_rgb_underglow_tick_handler, NULL);

/* ========================================================================= */
/*  Settings                                                                 */
/* ========================================================================= */

#if IS_ENABLED(CONFIG_SETTINGS)
static int rgb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    int rc;

    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(state)) {
            return -EINVAL;
        }

        rc = read_cb(cb_arg, &state, sizeof(state));
        if (rc >= 0) {
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
            /* Migrate: if saved state had old LAYER_INDICATORS effect, reset */
            if (state.current_effect >= UNDERGLOW_EFFECT_NUMBER) {
                state.current_effect = UNDERGLOW_EFFECT_SOLID;
            }
#endif
            if (state.on) {
                k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(1000 / ANIMATION_FPS));
            }
            return 0;
        }
        return rc;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(rgb_underglow, "rgb/underglow", NULL, rgb_settings_set, NULL, NULL);

static void zmk_rgb_underglow_save_state_work(struct k_work *_work) {
    settings_save_one("rgb/underglow/state", &state, sizeof(state));
}

static struct k_work_delayable underglow_save_work;
#endif

/* ========================================================================= */
/*  Init                                                                     */
/* ========================================================================= */

static int zmk_rgb_underglow_init(void) {
    led_strip = DEVICE_DT_GET(STRIP_CHOSEN);

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (!device_is_ready(ext_power)) {
        LOG_ERR("External power device \"%s\" is not ready", ext_power->name);
        return -ENODEV;
    }
#endif

    state = (struct rgb_underglow_state){
        color : {
            h : CONFIG_ZMK_RGB_UNDERGLOW_HUE_START,
            s : CONFIG_ZMK_RGB_UNDERGLOW_SAT_START,
            b : CONFIG_ZMK_RGB_UNDERGLOW_BRT_START,
        },
        animation_speed : CONFIG_ZMK_RGB_UNDERGLOW_SPD_START,
        current_effect : CONFIG_ZMK_RGB_UNDERGLOW_EFF_START,
        animation_step : 0,
        on : IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_ON_START)
    };

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&underglow_save_work, zmk_rgb_underglow_save_state_work);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
    state.on = zmk_usb_is_powered();
#endif

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    /* Migrate: if saved state had old LAYER_INDICATORS effect, reset to SOLID */
    if (state.current_effect >= UNDERGLOW_EFFECT_NUMBER) {
        state.current_effect = UNDERGLOW_EFFECT_SOLID;
    }
#endif

    /* Build inverse pixel lookup table: key position → LED strip index */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int key_pos = effect_pixel_lookup(i);
        if (key_pos >= 0 && key_pos < STRIP_NUM_PIXELS) {
            key_to_pixel[key_pos] = (uint8_t)i;
        }
    }

    /* Build per-LED normalised coordinate arrays from rgb_transform */
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (rgb_has_led_positions()) {
        int min_x = 999999, max_x = -999999;
        int min_y = 999999, max_y = -999999;
        /* First pass: find coordinate bounds */
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            int bidx = effect_pixel_lookup(i);
            int x = rgb_led_position_raw_x(bidx);
            int y = rgb_led_position_raw_y(bidx);
            if (x < min_x)
                min_x = x;
            if (x > max_x)
                max_x = x;
            if (y < min_y)
                min_y = y;
            if (y > max_y)
                max_y = y;
        }
        int range_x = (max_x - min_x);
        int range_y = (max_y - min_y);
        if (range_x == 0)
            range_x = 1;
        if (range_y == 0)
            range_y = 1;
        /* Second pass: normalise to 0-255 */
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            int bidx = effect_pixel_lookup(i);
            led_pos_x[i] = (uint8_t)((rgb_led_position_raw_x(bidx) - min_x) * 255 / range_x);
            led_pos_y[i] = (uint8_t)((rgb_led_position_raw_y(bidx) - min_y) * 255 / range_y);
        }
        positions_available = true;
        LOG_DBG("RGB: LED positions loaded (%d LEDs, range x=%d y=%d)", STRIP_NUM_PIXELS, range_x,
                range_y);
    }
#endif

    /* Initialize ripple mutex BEFORE any reset that may lock it */
    k_mutex_init(&ripple_mutex);

    /* Initialize all effect state */
    for (int i = 0; i < UNDERGLOW_EFFECT_NUMBER; i++) {
        if (effect_table[i].reset)
            effect_table[i].reset();
    }

    if (state.on) {
        k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(1000 / ANIMATION_FPS));
    }
    return 0;
}

/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */

int zmk_rgb_underglow_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    int ret = k_work_reschedule(&underglow_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
#else
    return 0;
#endif
}

int zmk_rgb_underglow_get_state(bool *on_off) {
    if (!led_strip)
        return -ENODEV;
    *on_off = state.on;
    return 0;
}

int zmk_rgb_underglow_on(void) {
    zmk_rgb_underglow_transient_on();
    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_transient_on(void) {
    if (!led_strip)
        return -ENODEV;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_enable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to enable EXT_POWER: %d", rc);
        }
    }
#endif

    state.on = true;
    state.animation_step = 0;
    /* Reset all effect state */
    for (int i = 0; i < UNDERGLOW_EFFECT_NUMBER; i++) {
        if (effect_table[i].reset)
            effect_table[i].reset();
    }
    k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(1000 / ANIMATION_FPS));
    return 0;
}

static void zmk_rgb_underglow_off_handler(struct k_work *work) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }
    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
}

K_WORK_DEFINE(underglow_off_work, zmk_rgb_underglow_off_handler);

int zmk_rgb_underglow_off(void) {
    zmk_rgb_underglow_transient_off();
    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_transient_off(void) {
    if (!led_strip)
        return -ENODEV;

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_disable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to disable EXT_POWER: %d", rc);
        }
    }
#endif

    state.on = false;
    k_timer_stop(&underglow_tick);
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_off_work);

    return 0;
}

int zmk_rgb_underglow_calc_effect(int direction) {
    return (state.current_effect + UNDERGLOW_EFFECT_NUMBER + direction) % UNDERGLOW_EFFECT_NUMBER;
}

int zmk_rgb_underglow_select_effect(int effect) {
    if (!led_strip)
        return -ENODEV;
    if (effect < 0 || effect >= UNDERGLOW_EFFECT_NUMBER)
        return -EINVAL;

    state.current_effect = effect;
    state.animation_step = 0;

    /* Reset all effect state and purge stale keypress events */
    k_msgq_purge(&effect_event_msgq);
    dither_reset();
    for (int i = 0; i < UNDERGLOW_EFFECT_NUMBER; i++) {
        if (effect_table[i].reset)
            effect_table[i].reset();
    }

    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_cycle_effect(int direction) {
    return zmk_rgb_underglow_select_effect(zmk_rgb_underglow_calc_effect(direction));
}

int zmk_rgb_underglow_toggle(void) {
    return state.on ? zmk_rgb_underglow_off() : zmk_rgb_underglow_on();
}

/* Layer indicator support functions are now integrated into the tick via
 * zmk_rgb_underglow_apply_layer_overlay() above. */

/* ========================================================================= */
/*  HSB Controls (public API, compatible with behavior_rgb_underglow.c)      */
/* ========================================================================= */

int zmk_rgb_underglow_set_hsb(struct zmk_led_hsb color) {
    if (color.h > HUE_MAX || color.s > SAT_MAX || color.b > BRT_MAX) {
        return -ENOTSUP;
    }
    state.color = color;
    return 0;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_hue(int direction) {
    struct zmk_led_hsb color = state.color;
    color.h += HUE_MAX + (direction * CONFIG_ZMK_RGB_UNDERGLOW_HUE_STEP);
    color.h %= HUE_MAX;
    return color;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_sat(int direction) {
    struct zmk_led_hsb color = state.color;
    int s = color.s + (direction * CONFIG_ZMK_RGB_UNDERGLOW_SAT_STEP);
    if (s < 0)
        s = 0;
    else if (s > SAT_MAX)
        s = SAT_MAX;
    color.s = s;
    return color;
}

struct zmk_led_hsb zmk_rgb_underglow_calc_brt(int direction) {
    struct zmk_led_hsb color = state.color;
    int b = color.b + (direction * CONFIG_ZMK_RGB_UNDERGLOW_BRT_STEP);
    color.b = CLAMP(b, 0, BRT_MAX);
    return color;
}

int zmk_rgb_underglow_change_hue(int direction) {
    if (!led_strip)
        return -ENODEV;
    state.color = zmk_rgb_underglow_calc_hue(direction);
    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_sat(int direction) {
    if (!led_strip)
        return -ENODEV;
    state.color = zmk_rgb_underglow_calc_sat(direction);
    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_brt(int direction) {
    if (!led_strip)
        return -ENODEV;
    state.color = zmk_rgb_underglow_calc_brt(direction);
    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_spd(int direction) {
    if (!led_strip)
        return -ENODEV;
    if (state.animation_speed == 1 && direction < 0)
        return 0;
    state.animation_speed += direction;
    if (state.animation_speed > 10)
        state.animation_speed = 10;
    return zmk_rgb_underglow_save_state();
}

struct zmk_led_hsb zmk_rgb_underglow_get_hsb(void) {
    return state.color;
}

int zmk_rgb_underglow_get_effect(void) {
    return (int)state.current_effect;
}

int zmk_rgb_underglow_get_speed(void) {
    return (int)state.animation_speed;
}

int zmk_rgb_underglow_set_speed(int speed) {
    if (!led_strip)
        return -ENODEV;
    if (speed < 1)
        speed = 1;
    if (speed > 10)
        speed = 10;
    state.animation_speed = (uint8_t)speed;
    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_get_effect_count(void) {
    return UNDERGLOW_EFFECT_NUMBER;
}

/* ========================================================================= */
/*  Event Listeners                                                          */
/* ========================================================================= */

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE) ||                                          \
    IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB) || IS_ENABLED(UNDERGLOW_LAYER_ENABLED)

static struct {
    bool rgb_state_before_auto_off; /* user's RGB on/off before any auto-off kicked in */
    bool saved;                     /* true once we've captured the user state */
    bool idle_wants_off;            /* idle triggered auto-off */
    bool usb_wants_off;             /* USB disconnect triggered auto-off */
} auto_off_state;

static bool auto_off_any_active(void) {
    return auto_off_state.idle_wants_off || auto_off_state.usb_wants_off;
}

static int rgb_underglow_auto_update(void) {
    bool should_off = auto_off_any_active();

    if (should_off) {
        if (!auto_off_state.saved) {
            auto_off_state.rgb_state_before_auto_off = state.on;
            auto_off_state.saved = true;
        }
        return zmk_rgb_underglow_transient_off();
    } else {
        if (auto_off_state.saved) {
            auto_off_state.saved = false;
            if (auto_off_state.rgb_state_before_auto_off)
                return zmk_rgb_underglow_transient_on();
            else
                return zmk_rgb_underglow_transient_off();
        }
        return 0;
    }
}
#endif

static int rgb_underglow_event_listener(const zmk_event_t *eh) {

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
    if (as_zmk_activity_state_changed(eh)) {
        auto_off_state.idle_wants_off = (zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE);
        return rgb_underglow_auto_update();
    }
#endif

    /* Layer overlay is applied in the tick automatically;
     * layer_state_changed and underglow_color_changed events
     * are handled implicitly since the overlay reads current state each frame. */

    /* Handle keypress events for key-interactive effects (ripple, reactive, etc.) */
    if (as_zmk_position_state_changed(eh)) {
        const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
        if (ev->state && state.on && effect_table[state.current_effect].on_keypress) {
            struct effect_event e = {.position = ev->position};
            /* Non-blocking enqueue; drop if full to avoid blocking event context */
            (void)k_msgq_put(&effect_event_msgq, &e, K_NO_WAIT);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        auto_off_state.usb_wants_off = !zmk_usb_is_powered();
        return rgb_underglow_auto_update();
    }
#endif

    return -ENOTSUP;
}

ZMK_LISTENER(rgb_underglow, rgb_underglow_event_listener);

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_activity_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_usb_conn_state_changed);
#endif

/* Layer overlay is handled in the tick; no explicit subscriptions needed. */

/* Subscribe to key position events for key-interactive effects */
ZMK_SUBSCRIPTION(rgb_underglow, zmk_position_state_changed);

SYS_INIT(zmk_rgb_underglow_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
