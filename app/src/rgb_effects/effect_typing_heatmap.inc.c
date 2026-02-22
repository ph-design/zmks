/* TYPING HEATMAP effect: keys "heat up" on press and their heat spreads
 * to neighbouring LEDs.  Colour goes from off → blue → green → yellow → red
 * as temperature increases.  Temperature decays over time.
 *
 * Uses physical 2D coordinates for proper neighbour spreading when
 * led-positions are available (QMK-style).  Falls back to 1D strip
 * neighbours when not.
 *
 * Inspired by QMK RGB_MATRIX_TYPING_HEATMAP.
 */

/* Per-pixel temperature: 0 = cold, 255 = hottest */
static uint8_t heatmap_temp[STRIP_NUM_PIXELS];

/* Millisecond timestamp of last decay tick */
static int64_t heatmap_last_decay_ms;

#define HEATMAP_INCREASE_STEP 60    /* heat added on press */
#define HEATMAP_SPREAD_RADIUS 0.15f /* normalised distance for heat spread */
#define HEATMAP_SPREAD_AMOUNT 20    /* max heat per spread */
#define HEATMAP_DECAY_INTERVAL 40   /* ms between decay ticks */
#define HEATMAP_DECAY_AMOUNT 1      /* temperature lost per decay tick */

static void heatmap_add_event(uint32_t position) {
    if (position >= STRIP_NUM_PIXELS)
        return;

    /* Heat all LEDs mapped to this key (multi-LED support) */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        if ((uint32_t)effect_pixel_lookup(i) == position) {
            int t = (int)heatmap_temp[i] + HEATMAP_INCREASE_STEP;
            heatmap_temp[i] = (uint8_t)(t > 255 ? 255 : t);
        }
    }

    /* Spread heat to nearby LEDs using 2D coordinates */
    float sx = key_src_x(position);
    float sy = key_src_y(position);

    for (int j = 0; j < STRIP_NUM_PIXELS; j++) {
        if ((uint32_t)effect_pixel_lookup(j) == position)
            continue; /* skip the key itself */

        float dx = led_norm_x(j) - sx;
        float dy = led_norm_y(j) - sy;
        float dist = sqrtf(dx * dx + dy * dy);

        if (dist < HEATMAP_SPREAD_RADIUS) {
            int amount = (int)(HEATMAP_SPREAD_AMOUNT * (1.0f - dist / HEATMAP_SPREAD_RADIUS));
            if (amount > 0) {
                int v = (int)heatmap_temp[j] + amount;
                heatmap_temp[j] = (uint8_t)(v > 255 ? 255 : v);
            }
        }
    }
}

static void heatmap_reset(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++)
        heatmap_temp[i] = 0;
    heatmap_last_decay_ms = k_uptime_get();
}

/* Map temperature 0-255 to HSL hue (240 blue → 0 red) */
static void heatmap_temp_to_rgb(uint8_t temp, struct color_rgb_float *out, float brt) {
    if (temp == 0) {
        *out = (struct color_rgb_float){0, 0, 0};
        return;
    }

    /* Map: 0→hue 240(blue), 128→hue 60(yellow), 255→hue 0(red) */
    float t = (float)temp / 255.0f;
    uint16_t hue;
    if (t < 0.5f) {
        /* Blue (240) → Green (120) */
        hue = 240 - (uint16_t)(t * 2.0f * 120.0f);
    } else {
        /* Green (120) → Red (0) */
        hue = 120 - (uint16_t)((t - 0.5f) * 2.0f * 120.0f);
    }

    struct color_hsl hsl = {hue, 100, 50};
    struct color_rgb_float rgb;
    hsl_to_rgb_float(&hsl, &rgb);

    /* Intensity scales with temperature */
    float intensity = t * brt;
    out->r = rgb.r * intensity;
    out->g = rgb.g * intensity;
    out->b = rgb.b * intensity;
}

static void zmk_rgb_underglow_effect_typing_heatmap(void) {
    float brt = get_brightness_factor();

    /* Time-based decay */
    int64_t now = k_uptime_get();
    int64_t elapsed = now - heatmap_last_decay_ms;
    /* Speed affects decay rate */
    int decay_interval =
        HEATMAP_DECAY_INTERVAL / (state.animation_speed > 0 ? state.animation_speed : 1);
    if (decay_interval < 10)
        decay_interval = 10;

    if (elapsed >= decay_interval) {
        int ticks = (int)(elapsed / decay_interval);
        int total_decay = ticks * HEATMAP_DECAY_AMOUNT;
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            int v = (int)heatmap_temp[i] - total_decay;
            heatmap_temp[i] = (uint8_t)(v < 0 ? 0 : v);
        }
        heatmap_last_decay_ms = now;
    }

    /* Render */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        heatmap_temp_to_rgb(heatmap_temp[i], &fx_pixels[i], brt);
    }
}
