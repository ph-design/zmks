/* TYPING HEATMAP — ported from QMK RGB_MATRIX_TYPING_HEATMAP. */

static uint8_t heatmap_temp[STRIP_NUM_PIXELS];
static uint32_t heatmap_decrease_timer;
static bool heatmap_decrease_this_frame;

#ifndef HEATMAP_INCREASE_STEP
#define HEATMAP_INCREASE_STEP 32
#endif

#ifndef HEATMAP_DECREASE_DELAY_MS
#define HEATMAP_DECREASE_DELAY_MS 50
#endif

#ifndef HEATMAP_SPREAD
#define HEATMAP_SPREAD 0.28f
#endif

#ifndef HEATMAP_AREA_LIMIT
#define HEATMAP_AREA_LIMIT 24
#endif

static void heatmap_add_event(uint32_t position) {
    if (position >= STRIP_NUM_PIXELS)
        return;

    uint8_t pressed_led = key_to_pixel[position];
    int t = (int)heatmap_temp[pressed_led] + HEATMAP_INCREASE_STEP;
    heatmap_temp[pressed_led] = (uint8_t)(t > 255 ? 255 : t);

    float src_x = key_src_x(position);
    float src_y = key_src_y(position);

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        if (i == pressed_led)
            continue;

        float dx = led_norm_x(i) - src_x;
        float dy = led_norm_y(i) - src_y;
        float dist = sqrtf(dx * dx + dy * dy);

        if (dist <= HEATMAP_SPREAD) {
            uint8_t amount = (uint8_t)((1.0f - dist / HEATMAP_SPREAD) * HEATMAP_AREA_LIMIT);
            if (amount > HEATMAP_AREA_LIMIT)
                amount = HEATMAP_AREA_LIMIT;
            if (amount > 0) {
                int v = (int)heatmap_temp[i] + amount;
                heatmap_temp[i] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
    }
}

static void heatmap_reset(void) {
    memset(heatmap_temp, 0, sizeof(heatmap_temp));
    heatmap_decrease_timer = (uint32_t)k_uptime_get();
}

static void zmk_rgb_underglow_effect_typing_heatmap(void) {
    float brt = get_brightness_factor();

    uint32_t now = (uint32_t)k_uptime_get();
    int delay_ms =
        HEATMAP_DECREASE_DELAY_MS / (state.animation_speed > 0 ? state.animation_speed : 1);
    if (delay_ms < 5)
        delay_ms = 5;

    heatmap_decrease_this_frame = ((now - heatmap_decrease_timer) >= (uint32_t)delay_ms);
    if (heatmap_decrease_this_frame)
        heatmap_decrease_timer = now;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t val = heatmap_temp[i];

        if (heatmap_decrease_this_frame && val > 0)
            heatmap_temp[i] = --val;

        if (val == 0) {
            fx_pixels[i] = (struct color_rgb_float){0, 0, 0};
            continue;
        }

        /* Smooth heatmap gradient: blue → cyan → green → yellow → red
         * 5 stops evenly spaced across val 1-255 for perceptually balanced transitions. */
        float t = (float)val / 255.0f;
        float r, g, b;
        if (t < 0.25f) {
            /* blue → cyan */
            float s = t / 0.25f;
            r = 0.0f;
            g = s;
            b = 1.0f;
        } else if (t < 0.5f) {
            /* cyan → green */
            float s = (t - 0.25f) / 0.25f;
            r = 0.0f;
            g = 1.0f;
            b = 1.0f - s;
        } else if (t < 0.75f) {
            /* green → yellow */
            float s = (t - 0.5f) / 0.25f;
            r = s;
            g = 1.0f;
            b = 0.0f;
        } else {
            /* yellow → red */
            float s = (t - 0.75f) / 0.25f;
            r = 1.0f;
            g = 1.0f - s;
            b = 0.0f;
        }

        /* Brightness: ramp up for low val, full at val≥85 */
        float intensity = (val >= 85 ? 1.0f : (float)val / 85.0f) * brt;

        fx_pixels[i].r = r * intensity;
        fx_pixels[i].g = g * intensity;
        fx_pixels[i].b = b * intensity;
    }
}
