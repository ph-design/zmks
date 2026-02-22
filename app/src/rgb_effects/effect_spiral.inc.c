/* SPIRAL effect (moving center gradient) — uses 2D physical coordinates */
static float spiral_phase = 0.0f;
static void zmk_rgb_underglow_effect_spiral(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();

    /* Center point moves along X axis */
    float cx = spiral_phase;
    float cy = 0.5f; /* vertical center */

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        float dx = led_norm_x(i) - cx;
        float dy = led_norm_y(i) - cy;
        float dist = sqrtf(dx * dx + dy * dy);
        /* Normalise: max possible distance ~= 1.0 */
        float v = 1.0f - dist * 2.0f;
        v = CLAMP(v, 0.0f, 1.0f);
        struct color_rgb_float rgb;
        hsl_to_rgb_float(&hsl, &rgb);
        rgb.r *= brt * v;
        rgb.g *= brt * v;
        rgb.b *= brt * v;
        fx_pixels[i] = rgb;
    }

    spiral_phase += (float)state.animation_speed * 0.005f;
    if (spiral_phase >= 1.0f)
        spiral_phase -= 1.0f;
}

static void spiral_reset(void) { spiral_phase = 0.0f; }
