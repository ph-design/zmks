/* BREATHING effect */
static float breathing_phase = 0.0f;

/* Fast sine approximation (Bhaskara I) — avoids libm sinf overhead */
static float fast_sin_01(float phase_01) {
    /* Map 0..1 phase to 0..2PI, return (sin+1)/2 in 0..1 */
    float x = phase_01 * 2.0f;
    if (x > 1.0f) x = 2.0f - x;
    /* Parabolic approximation: 4x(1-x) peaks at 1 when x=0.5 */
    return 4.0f * x * (1.0f - x);
}

static void zmk_rgb_underglow_effect_breathing(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float rgb;
    hsl_to_rgb_float(&hsl, &rgb);

    /* Smooth breathing using fast sine approximation */
    float val = fast_sin_01(breathing_phase); /* 0..1 */
    float brt = get_brightness_factor() * (0.15f + 0.85f * val);

    struct color_rgb_float out = {rgb.r * brt, rgb.g * brt, rgb.b * brt};
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i] = out;
    }

    /* Advance phase according to animation speed */
    breathing_phase += (float)state.animation_speed * 0.003f;
    if (breathing_phase >= 1.0f)
        breathing_phase -= 1.0f;
}

static void breathing_reset(void) {
    breathing_phase = 0.0f;
}
