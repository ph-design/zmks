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

struct color_hsl {
    uint16_t h;
    uint8_t s;
    uint8_t l;
};

static void hsl_to_rgb_float(const struct color_hsl *hsl, struct color_rgb_float *rgb) {
    float s = (float)hsl->s / 100;
    float l = (float)hsl->l / 100;
    float a = (float)hsl->h / 60;
    float chroma = s * (1 - fabsf(2 * l - 1));
    float x = chroma * (1 - fabsf(fmodf(a, 2) - 1));
    float m = l - chroma / 2;

    switch ((uint8_t)a % 6) {
    case 0:
        rgb->r = m + chroma;
        rgb->g = m + x;
        rgb->b = m;
        break;
    case 1:
        rgb->r = m + x;
        rgb->g = m + chroma;
        rgb->b = m;
        break;
    case 2:
        rgb->r = m;
        rgb->g = m + chroma;
        rgb->b = m + x;
        break;
    case 3:
        rgb->r = m;
        rgb->g = m + x;
        rgb->b = m + chroma;
        break;
    case 4:
        rgb->r = m + x;
        rgb->g = m;
        rgb->b = m + chroma;
        break;
    case 5:
        rgb->r = m + chroma;
        rgb->g = m;
        rgb->b = m + x;
        break;
    }
}

static void rgb_float_to_led(const struct color_rgb_float *rgb, struct led_rgb *led) {
    float r = rgb->r > 1.0f ? 1.0f : (rgb->r < 0 ? 0 : rgb->r);
    float g = rgb->g > 1.0f ? 1.0f : (rgb->g < 0 ? 0 : rgb->g);
    float b = rgb->b > 1.0f ? 1.0f : (rgb->b < 0 ? 0 : rgb->b);
    led->r = (uint8_t)(r * 255);
    led->g = (uint8_t)(g * 255);
    led->b = (uint8_t)(b * 255);
}

static void interpolate_hsl(const struct color_hsl *from, const struct color_hsl *to,
                            struct color_hsl *result, float step) {
    /* Shortest-path hue interpolation */
    int16_t hue_diff = (int16_t)to->h - (int16_t)from->h;
    if (hue_diff > 180)
        hue_diff -= 360;
    if (hue_diff < -180)
        hue_diff += 360;
    int16_t h = (int16_t)from->h + (int16_t)(hue_diff * step);
    if (h < 0)
        h += 360;
    if (h >= 360)
        h -= 360;
    result->h = (uint16_t)h;
    result->s = (uint8_t)(from->s + (int16_t)(to->s - from->s) * step);
    result->l = (uint8_t)(from->l + (int16_t)(to->l - from->l) * step);
}

static void interpolate_rgb(const struct color_rgb_float *from, const struct color_rgb_float *to,
                            struct color_rgb_float *result, float step) {
    result->r = from->r + (to->r - from->r) * step;
    result->g = from->g + (to->g - from->g) * step;
    result->b = from->b + (to->b - from->b) * step;
}

/* ========================================================================= */
/*  HSB conversion (existing, for layer indicators & user color controls)    */
/* ========================================================================= */

static struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb) {
    hsb.b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN +
            (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX - CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN) * hsb.b / BRT_MAX;
    return hsb;
}

static struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb) {
    float r = 0, g = 0, b = 0;
    uint8_t i = hsb.h / 60;
    float v = hsb.b / ((float)BRT_MAX);
    float s = hsb.s / ((float)SAT_MAX);
    float f = hsb.h / ((float)HUE_MAX) * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    switch (i % 6) {
    case 0:
        r = v;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = v;
        b = p;
        break;
    case 2:
        r = p;
        g = v;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = v;
        break;
    case 4:
        r = t;
        g = p;
        b = v;
        break;
    case 5:
        r = v;
        g = p;
        b = q;
        break;
    }

    struct led_rgb rgb = {r : r * 255, g : g * 255, b : b * 255};
    return rgb;
}

/* Convert user HSB to HSL for effects that use HSL internally */
static struct color_hsl hsb_to_hsl(struct zmk_led_hsb hsb) {
    struct color_hsl hsl;
    hsl.h = hsb.h;
    hsl.s = hsb.s;
    /* HSB brightness -> HSL lightness: L = B * (1 - S/200) */
    float b_f = (float)hsb.b / 100.0f;
    float s_f = (float)hsb.s / 100.0f;
    float l = b_f * (1.0f - s_f / 2.0f);
    hsl.l = (uint8_t)(l * 100);
    if (hsl.l < 1 && hsb.b > 0)
        hsl.l = 1;
    return hsl;
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
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float rgb;
    hsl_to_rgb_float(&hsl, &rgb);
    sparkle_data[idx].color = rgb;
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
/* ── Static ─────────────────────────────────────────── */
#include "rgb_effects/effect_solid.inc.c"

/* ── Simple animation ───────────────────────────────── */
#include "rgb_effects/effect_breathing.inc.c"

/* ── Hue-based animations ───────────────────────────── */
#include "rgb_effects/effect_spectrum.inc.c"
#include "rgb_effects/effect_rainbow.inc.c"
#include "rgb_effects/effect_gradient.inc.c"

/* ── Pattern / spatial animations ───────────────────── */
#include "rgb_effects/effect_wave.inc.c"
#include "rgb_effects/effect_knight.inc.c"
#include "rgb_effects/effect_twinkle.inc.c"
#include "rgb_effects/effect_sparkle.inc.c"
#include "rgb_effects/effect_raindrops.inc.c"

/* ── Hybrid / static-split ──────────────────────────── */
#include "rgb_effects/effect_alphas_mods.inc.c"

/* ── Key-interactive effects ────────────────────────── */
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
    /* ── Static ─────────────────────────────────────────── */
    [UNDERGLOW_EFFECT_SOLID] = {.render = zmk_rgb_underglow_effect_solid},

    /* ── Simple animation ───────────────────────────────── */
    [UNDERGLOW_EFFECT_BREATHING] = {.render = zmk_rgb_underglow_effect_breathing,
                                    .reset = breathing_reset},

    /* ── Hue-based animations ───────────────────────────── */
    [UNDERGLOW_EFFECT_SPECTRUM] = {.render = zmk_rgb_underglow_effect_spectrum,
                                   .reset = spectrum_reset},
    [UNDERGLOW_EFFECT_RAINBOW] = {.render = zmk_rgb_underglow_effect_rainbow,
                                  .reset = rainbow_reset},
    [UNDERGLOW_EFFECT_GRADIENT] = {.render = zmk_rgb_underglow_effect_gradient,
                                   .reset = gradient_reset},

    /* ── Pattern / spatial animations ───────────────────── */
    [UNDERGLOW_EFFECT_WAVE] = {.render = zmk_rgb_underglow_effect_wave, .reset = wave_reset},
    [UNDERGLOW_EFFECT_KNIGHT] = {.render = zmk_rgb_underglow_effect_knight, .reset = knight_reset},
    [UNDERGLOW_EFFECT_TWINKLE] = {.render = zmk_rgb_underglow_effect_twinkle,
                                  .reset = twinkle_reset},
    [UNDERGLOW_EFFECT_SPARKLE] = {.render = zmk_rgb_underglow_effect_sparkle,
                                  .reset = sparkle_init_all},
    [UNDERGLOW_EFFECT_RAINDROPS] = {.render = zmk_rgb_underglow_effect_raindrops,
                                    .reset = raindrops_reset},

    /* ── Hybrid / static-split ──────────────────────────── */
    [UNDERGLOW_EFFECT_ALPHAS_MODS] = {.render = zmk_rgb_underglow_effect_alphas_mods,
                                      .reset = alphas_mods_reset},

    /* ── Key-interactive effects ────────────────────────── */
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
    struct zmk_led_hsb hsb = state.color;
    return (struct led_rgb){
        r : (hsb.b * (r)) / 0xff,
        g : (hsb.b * (g)) / 0xff,
        b : (hsb.b * (b)) / 0xff
    };
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
        rgb_float_to_led(&fx_pixels[i], &pixels[i]);
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
