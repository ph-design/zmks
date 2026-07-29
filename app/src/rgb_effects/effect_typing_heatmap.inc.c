/* TYPING HEATMAP — ported from QMK RGB_MATRIX_TYPING_HEATMAP. */

static uint8_t heatmap_temp[STRIP_NUM_PIXELS];
static uint32_t heatmap_decrease_timer;

#ifndef HEATMAP_INCREASE_STEP
#define HEATMAP_INCREASE_STEP 32
#endif

#ifndef HEATMAP_DECREASE_DELAY_MS
#define HEATMAP_DECREASE_DELAY_MS 50
#endif

#ifndef HEATMAP_SPREAD_X
#define HEATMAP_SPREAD_X 0.28f
#endif

#ifndef HEATMAP_SPREAD_Y
#define HEATMAP_SPREAD_Y 0.45f
#endif

#ifndef HEATMAP_AREA_LIMIT
#define HEATMAP_AREA_LIMIT 24
#endif

static void heatmap_add_event(uint32_t position) {
    if (position >= STRIP_NUM_PIXELS || key_to_pixel[position] == 0xFF)
        return;

    float src_x = key_src_x(position);
    float src_y = key_src_y(position);

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        if ((uint32_t)effect_pixel_lookup(i) == position) {
            int t = (int)heatmap_temp[i] + HEATMAP_INCREASE_STEP;
            heatmap_temp[i] = (uint8_t)(t > 255 ? 255 : t);
            continue;
        }

        float nx = (led_norm_x(i) - src_x) / HEATMAP_SPREAD_X;
        float ny = (led_norm_y(i) - src_y) / HEATMAP_SPREAD_Y;
        float nd = sqrtf(nx * nx + ny * ny);

        if (nd <= 1.0f) {
            uint8_t amount = (uint8_t)((1.0f - nd) * HEATMAP_AREA_LIMIT);
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
        (int)(HEATMAP_DECREASE_DELAY_MS / anim_speed());
    if (delay_ms < 5)
        delay_ms = 5;

    bool decrease = ((now - heatmap_decrease_timer) >= (uint32_t)delay_ms);
    if (decrease)
        heatmap_decrease_timer = now;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t val = heatmap_temp[i];

        if (decrease && val > 0)
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
