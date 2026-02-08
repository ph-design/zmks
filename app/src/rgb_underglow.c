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
static void zmk_rgb_underglow_set_layer(uint8_t layer, bool wakeup);
#endif

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

static float float_mod(float a, float b) {
    float mod = a < 0 ? -a : a;
    float x = b < 0 ? -b : b;
    while (mod >= x) {
        mod = mod - x;
    }
    return a < 0 ? -mod : mod;
}

static float float_abs(float a) { return a < 0 ? -a : a; }

static void hsl_to_rgb_float(const struct color_hsl *hsl, struct color_rgb_float *rgb) {
    float s = (float)hsl->s / 100;
    float l = (float)hsl->l / 100;
    float a = (float)hsl->h / 60;
    float chroma = s * (1 - float_abs(2 * l - 1));
    float x = chroma * (1 - float_abs(float_mod(a, 2) - 1));
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
    UNDERGLOW_EFFECT_SOLID,    /* Solid with multi-color HSL cycling */
    UNDERGLOW_EFFECT_GRADIENT, /* Linear gradient with scrolling */
    UNDERGLOW_EFFECT_SPARKLE,  /* Random sparkle */
    UNDERGLOW_EFFECT_RIPPLE,   /* Keypress ripple */
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    UNDERGLOW_EFFECT_LAYER_INDICATORS,
#endif
    UNDERGLOW_EFFECT_NUMBER
};

struct rgb_underglow_state {
    struct zmk_led_hsb color;
    uint8_t animation_speed;
    uint8_t current_effect;
    uint16_t animation_step;
    bool on;
    bool layer_enabled;
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
/*  Ripple Effect State (ported from zmk-rgb-fx/ripple.c)                    */
/* ========================================================================= */

#define RIPPLE_MAX_EVENTS 8
#define RIPPLE_WIDTH 40

struct ripple_event {
    uint32_t pixel_id;
    uint16_t distance;
    uint8_t counter;
};

static struct ripple_event ripple_events[RIPPLE_MAX_EVENTS];
static uint8_t ripple_events_start = 0;
static uint8_t ripple_events_end = 0;
static uint8_t ripple_num_events = 0;

/* ========================================================================= */
/*  Gradient & Solid Effect State                                            */
/* ========================================================================= */

static float gradient_offset = 0.0f;
static uint16_t solid_counter = 0;

/* ========================================================================= */
/*  Effect Rendering Functions                                               */
/* ========================================================================= */

/*
 * SOLID effect (ported from zmk-rgb-fx/solid.c)
 * Shows a solid color. When animation_speed > 1, smoothly cycles hue
 * creating a breathing color transition effect.
 */
static void zmk_rgb_underglow_effect_solid(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float rgb;
    float brt = get_brightness_factor();

    if (state.animation_speed > 1) {
        /* Cycle through complementary hue with HSL interpolation */
        uint16_t cycle_duration = ANIMATION_FPS * 10 / state.animation_speed;
        if (cycle_duration < 1)
            cycle_duration = 1;

        struct color_hsl hsl2 = hsl;
        hsl2.h = (hsl.h + 180) % 360;

        struct color_hsl interp;
        float step = (float)(solid_counter % cycle_duration) / (float)cycle_duration;
        /* Triangle wave: 0 -> 1 -> 0 for smooth back-and-forth */
        if (step > 0.5f)
            step = 1.0f - step;
        step *= 2.0f;
        interpolate_hsl(&hsl, &hsl2, &interp, step);
        hsl_to_rgb_float(&interp, &rgb);

        solid_counter++;
        if (solid_counter >= cycle_duration)
            solid_counter = 0;
    } else {
        hsl_to_rgb_float(&hsl, &rgb);
    }

    rgb.r *= brt;
    rgb.g *= brt;
    rgb.b *= brt;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i] = rgb;
    }
}

/*
 * LINEAR GRADIENT effect (ported from zmk-rgb-fx/linear_gradient.c)
 * Creates a scrolling 3-color gradient (base + 2 triadic colors) across the strip.
 */
static void zmk_rgb_underglow_effect_gradient(void) {
    struct color_hsl hsl1 = hsb_to_hsl(state.color);
    struct color_hsl hsl2 = hsl1;
    hsl2.h = (hsl1.h + 120) % 360;
    struct color_hsl hsl3 = hsl1;
    hsl3.h = (hsl1.h + 240) % 360;

    struct color_rgb_float rgb1, rgb2, rgb3;
    hsl_to_rgb_float(&hsl1, &rgb1);
    hsl_to_rgb_float(&hsl2, &rgb2);
    hsl_to_rgb_float(&hsl3, &rgb3);

    float brt = get_brightness_factor();
    float gradient_width = (float)STRIP_NUM_PIXELS;
    struct color_rgb_float colors[3] = {rgb1, rgb2, rgb3};

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        float distance = float_mod(gradient_width + (float)i - gradient_offset, gradient_width);
        if (distance < 0)
            distance += gradient_width;

        float segment_width = gradient_width / 3.0f;
        int from_idx = (int)(distance / segment_width);
        if (from_idx > 2)
            from_idx = 2;
        float step = (distance - from_idx * segment_width) / segment_width;
        if (step > 1.0f)
            step = 1.0f;

        struct color_rgb_float result;
        interpolate_rgb(&colors[from_idx], &colors[(from_idx + 1) % 3], &result, step);

        result.r *= brt;
        result.g *= brt;
        result.b *= brt;
        fx_pixels[i] = result;
    }

    /* Scroll */
    gradient_offset += (float)state.animation_speed * 0.15f;
    if (gradient_offset >= gradient_width) {
        gradient_offset -= gradient_width;
    }
}

/*
 * SPARKLE effect (ported from zmk-rgb-fx/sparkle.c)
 * Random twinkling pixels that fade in and out independently.
 */
static void zmk_rgb_underglow_effect_sparkle(void) {
    float brt = get_brightness_factor();

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        if (sparkle_data[i].counter == 0) {
            sparkle_generate_pixel(i, false);
        }

        sparkle_data[i].counter--;

        float intensity;
        if (sparkle_data[i].total_frames <= sparkle_data[i].counter) {
            /* Fade in phase */
            intensity = 2.0f - (sparkle_data[i].step * (float)(sparkle_data[i].counter));
        } else {
            /* Fade out phase */
            intensity = sparkle_data[i].step * (float)(sparkle_data[i].counter);
        }
        if (intensity < 0)
            intensity = 0;
        if (intensity > 1.0f)
            intensity = 1.0f;

        fx_pixels[i].r = intensity * sparkle_data[i].color.r * brt;
        fx_pixels[i].g = intensity * sparkle_data[i].color.g * brt;
        fx_pixels[i].b = intensity * sparkle_data[i].color.b * brt;
    }
}

/*
 * RIPPLE effect (ported from zmk-rgb-fx/ripple.c)
 * When a key is pressed, a ripple wave expands outward from that LED position.
 * Uses simplified 1D distance based on pixel index difference.
 */
static void zmk_rgb_underglow_effect_ripple(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float base_color;
    hsl_to_rgb_float(&hsl, &base_color);
    float brt = get_brightness_factor();

    /* Clear to black */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i].r = 0;
        fx_pixels[i].g = 0;
        fx_pixels[i].b = 0;
    }

    /* Ripple speed scales with animation_speed */
    uint8_t distance_per_frame = 3 + state.animation_speed * 2;
    uint8_t event_frames = 255 / distance_per_frame;
    if (event_frames < 1)
        event_frames = 1;

    /* Render each active ripple event */
    uint8_t idx = ripple_events_start;
    int processed = 0;
    while (idx != ripple_events_end && processed < ripple_num_events) {
        struct ripple_event *event = &ripple_events[idx];

        for (int j = 0; j < STRIP_NUM_PIXELS; j++) {
            /* 1D distance: scale pixel index difference to 0-255 range */
            int pixel_dist =
                (int)(((float)abs((int)j - (int)event->pixel_id) / (float)STRIP_NUM_PIXELS) * 255);

            int diff = abs(pixel_dist - (int)event->distance);
            if (diff < RIPPLE_WIDTH) {
                float intensity = (1.0f - (float)diff / (float)RIPPLE_WIDTH) * brt;

                struct color_rgb_float color = {
                    .r = intensity * base_color.r,
                    .g = intensity * base_color.g,
                    .b = intensity * base_color.b,
                };

                /* Lighten blending: take max of each channel */
                if (color.r > fx_pixels[j].r)
                    fx_pixels[j].r = color.r;
                if (color.g > fx_pixels[j].g)
                    fx_pixels[j].g = color.g;
                if (color.b > fx_pixels[j].b)
                    fx_pixels[j].b = color.b;
            }
        }

        /* Advance the ripple */
        if (event->counter < event_frames) {
            event->distance += distance_per_frame;
            event->counter++;
        } else {
            ripple_events_start = (ripple_events_start + 1) % RIPPLE_MAX_EVENTS;
            ripple_num_events--;
        }

        idx = (idx + 1) % RIPPLE_MAX_EVENTS;
        processed++;
    }
}

static void ripple_add_event(uint32_t position) {
    if (ripple_num_events >= RIPPLE_MAX_EVENTS) {
        /* Drop oldest event */
        ripple_events_start = (ripple_events_start + 1) % RIPPLE_MAX_EVENTS;
        ripple_num_events--;
    }

    /* Map key position to pixel index */
    uint32_t pixel = position % STRIP_NUM_PIXELS;

    ripple_events[ripple_events_end].pixel_id = pixel;
    ripple_events[ripple_events_end].distance = 0;
    ripple_events[ripple_events_end].counter = 0;

    ripple_events_end = (ripple_events_end + 1) % RIPPLE_MAX_EVENTS;
    ripple_num_events++;
}

/* ========================================================================= */
/*  Layer Indicator Effect (kept from original)                              */
/* ========================================================================= */

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
static void zmk_rgb_underglow_effect_layer(void) {
    bool active = false;
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i].r -= state.animation_speed < pixels[i].r ? state.animation_speed : pixels[i].r;
        pixels[i].g -= state.animation_speed < pixels[i].g ? state.animation_speed : pixels[i].g;
        pixels[i].b -= state.animation_speed < pixels[i].b ? state.animation_speed : pixels[i].b;
        if (pixels[i].r || pixels[i].g || pixels[i].b) {
            active = true;
        }
    }
    state.animation_step += state.animation_speed;
    if (state.animation_step > 255 || !active) {
        zmk_rgb_underglow_transient_off();
    }
}
#endif

/* ========================================================================= */
/*  Tick / Timer                                                             */
/* ========================================================================= */

static void zmk_rgb_underglow_tick(struct k_work *work) {
    switch (state.current_effect) {
    case UNDERGLOW_EFFECT_SOLID:
        zmk_rgb_underglow_effect_solid();
        break;
    case UNDERGLOW_EFFECT_GRADIENT:
        zmk_rgb_underglow_effect_gradient();
        break;
    case UNDERGLOW_EFFECT_SPARKLE:
        zmk_rgb_underglow_effect_sparkle();
        break;
    case UNDERGLOW_EFFECT_RIPPLE:
        zmk_rgb_underglow_effect_ripple();
        break;
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    case UNDERGLOW_EFFECT_LAYER_INDICATORS:
        zmk_rgb_underglow_effect_layer();
        /* Layer effect writes directly to pixels[], skip float conversion */
        goto update_strip;
#endif
    }

    /* Convert float pixels to LED strip format */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        rgb_float_to_led(&fx_pixels[i], &pixels[i]);
    }

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
update_strip:
#endif
{
    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }
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
            if (state.on) {
                k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(1000 / ANIMATION_FPS));
            }
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
            if (state.layer_enabled) {
                zmk_rgb_underglow_set_layer(rgb_underglow_top_layer(), true);
            }
#endif
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

static void sparkle_init_all(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        sparkle_generate_pixel(i, true);
    }
}

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

    sparkle_init_all();

    if (state.on) {
        k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(1000 / ANIMATION_FPS));
    }
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (state.layer_enabled) {
        zmk_rgb_underglow_set_layer(rgb_underglow_top_layer(), true);
    }
#endif
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
    *on_off = state.on || state.layer_enabled;
    return 0;
}

int zmk_rgb_underglow_on(void) {
    zmk_rgb_underglow_transient_on();
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (state.current_effect == UNDERGLOW_EFFECT_LAYER_INDICATORS) {
        state.layer_enabled = true;
        zmk_rgb_underglow_set_layer(rgb_underglow_top_layer(), true);
    }
#endif
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
    solid_counter = 0;
    gradient_offset = 0.0f;
    sparkle_init_all();
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
    state.layer_enabled = false;
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

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_off_work);
    k_timer_stop(&underglow_tick);
    state.on = false;
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
    solid_counter = 0;
    gradient_offset = 0.0f;
    sparkle_init_all();

    /* Reset ripple events */
    ripple_events_start = 0;
    ripple_events_end = 0;
    ripple_num_events = 0;

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    state.layer_enabled = (effect == UNDERGLOW_EFFECT_LAYER_INDICATORS);
    if (state.layer_enabled) {
        zmk_rgb_underglow_set_layer(rgb_underglow_top_layer(), true);
    }
#endif
    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_cycle_effect(int direction) {
    return zmk_rgb_underglow_select_effect(zmk_rgb_underglow_calc_effect(direction));
}

int zmk_rgb_underglow_toggle(void) {
    return state.on ? zmk_rgb_underglow_off() : zmk_rgb_underglow_on();
}

/* ========================================================================= */
/*  Layer Indicator Support (kept from original)                             */
/* ========================================================================= */

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)

static struct led_rgb hex_to_rgb(uint8_t r, uint8_t g, uint8_t b) {
    struct zmk_led_hsb hsb = state.color;
    return (struct led_rgb){
        r : (hsb.b * (r)) / 0xff,
        g : (hsb.b * (g)) / 0xff,
        b : (hsb.b * (b)) / 0xff
    };
}

static int zmk_rgb_underglow_apply_rgbmap(const struct zmk_behavior_binding *bindings,
                                          size_t rgbmap_len, uint8_t layer) {
    int rc = 0;
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t midx = rgb_pixel_lookup(i);
        if (midx >= ZMK_KEYMAP_LEN) {
            LOG_DBG("out of range");
        } else {
            const struct device *dev = zmk_behavior_get_binding(bindings[midx].behavior_dev);
            if (dev == NULL)
                continue;

            const struct behavior_driver_api *api = (const struct behavior_driver_api *)dev->api;
            if (api->binding_pressed == NULL)
                continue;

            struct zmk_behavior_binding_event event = {
                .position = midx, .layer = layer, .timestamp = k_uptime_get()};

            int color = api->binding_pressed((struct zmk_behavior_binding *)&bindings[midx], event);

            if (color > 0) {
                pixels[i] =
                    hex_to_rgb((color & 0xFF0000) >> 16, (color & 0xFF00) >> 8, color & 0xFF);
                rc = 1;
            } else {
                pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
            }
        }
    }
    return rc;
}

static void zmk_rgb_underglow_set_layer(uint8_t layer, bool wakeup) {
    LOG_DBG("state.layer: %d state.on: %d", state.layer_enabled, state.on);
    if (!state.layer_enabled)
        return;

    const struct zmk_behavior_binding *rgbmap = rgb_underglow_get_bindings(layer);
    if (rgbmap != NULL && zmk_rgb_underglow_apply_rgbmap(rgbmap, ZMK_KEYMAP_LEN, layer)) {
        if (!state.on) {
            if (!wakeup) {
                LOG_DBG("rgb off and no wakeup, abort refresh");
                return;
            }
            zmk_rgb_underglow_transient_on();
        }
        k_timer_stop(&underglow_tick);
        state.animation_step = 0;
        int fade_delay = zmk_rgbmap_fade_delay(layer);
        if (fade_delay >= 0) {
            k_timer_start(&underglow_tick, K_SECONDS(fade_delay), K_MSEC(1000 / ANIMATION_FPS));
        }
        LOG_DBG("write pixels");
        int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
        if (err < 0) {
            LOG_ERR("Failed to update the RGB strip (%d)", err);
        }
    } else {
        if (state.on)
            zmk_rgb_underglow_transient_off();
    }
}
#endif /* IS_ENABLED(UNDERGLOW_LAYER_ENABLED) */

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
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (state.layer_enabled) {
        zmk_rgb_underglow_set_layer(rgb_underglow_top_layer(), true);
    }
#endif
    return zmk_rgb_underglow_save_state();
}

int zmk_rgb_underglow_change_spd(int direction) {
    if (!led_strip)
        return -ENODEV;
    if (state.animation_speed == 1 && direction < 0)
        return 0;
    state.animation_speed += direction;
    if (state.animation_speed > 5)
        state.animation_speed = 5;
    return zmk_rgb_underglow_save_state();
}

/* ========================================================================= */
/*  Event Listeners                                                          */
/* ========================================================================= */

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE) ||                                          \
    IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB) || IS_ENABLED(UNDERGLOW_LAYER_ENABLED)

static int rgb_underglow_auto_state(bool target_wake_state) {
    static struct {
        bool is_awake;
        bool rgb_state_before_sleeping;
    } sleep_state = {.is_awake = true, .rgb_state_before_sleeping = false};

    if (target_wake_state == sleep_state.is_awake)
        return 0;
    sleep_state.is_awake = target_wake_state;

    if (sleep_state.is_awake) {
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
        if (state.layer_enabled) {
            zmk_rgb_underglow_set_layer(rgb_underglow_top_layer(), true);
            return 0;
        }
#endif
        if (sleep_state.rgb_state_before_sleeping)
            return zmk_rgb_underglow_transient_on();
        else
            return zmk_rgb_underglow_transient_off();
    } else {
        sleep_state.rgb_state_before_sleeping = state.on;
        return zmk_rgb_underglow_transient_off();
    }
}
#endif

static int rgb_underglow_event_listener(const zmk_event_t *eh) {

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_IDLE)
    if (as_zmk_activity_state_changed(eh)) {
        return rgb_underglow_auto_state(zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    }
#endif

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (as_zmk_layer_state_changed(eh)) {
        uint8_t layer = rgb_underglow_top_layer();
        LOG_DBG("layer_state_changed, top layer: %d", layer);
        zmk_rgb_underglow_set_layer(layer, true);
        return 0;
    }
    if (as_zmk_underglow_color_changed(eh)) {
        const struct zmk_underglow_color_changed *ev = as_zmk_underglow_color_changed(eh);
        uint8_t layer = rgb_underglow_top_layer();
        LOG_DBG("refresh layers %d, current: %d, wakeup: %d", ev->layers, layer, ev->wakeup);
        if ((ev->layers & (BIT(layer))) == BIT(layer)) {
            zmk_rgb_underglow_set_layer(rgb_underglow_top_layer(), ev->wakeup);
        }
        return 0;
    }
#endif

    /* Handle keypress events for ripple effect */
    if (as_zmk_position_state_changed(eh)) {
        const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
        if (ev->state && state.on && state.current_effect == UNDERGLOW_EFFECT_RIPPLE) {
            ripple_add_event(ev->position);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW_AUTO_OFF_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        return rgb_underglow_auto_state(zmk_usb_is_powered());
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

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
ZMK_SUBSCRIPTION(rgb_underglow, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(rgb_underglow, zmk_underglow_color_changed);
#endif

/* Subscribe to key position events for ripple effect */
ZMK_SUBSCRIPTION(rgb_underglow, zmk_position_state_changed);

SYS_INIT(zmk_rgb_underglow_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
