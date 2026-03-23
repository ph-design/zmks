/* BREATHING effect */
static float breathing_phase = 0.0f;

/* Parabolic pulse: maps 0..1 phase to a 0..1 bell curve (triangle→parabola). */
static float breath_pulse(float phase_01) {
    float x = phase_01 * 2.0f;
    if (x > 1.0f) x = 2.0f - x;
    return 4.0f * x * (1.0f - x);
}

static void zmk_rgb_underglow_effect_breathing(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float rgb;
    hsl_to_rgb_float(&hsl, &rgb);

    /* Smooth breathing using parabolic pulse */
    float val = breath_pulse(breathing_phase); /* 0..1 */
    /* Never go fully dark: keep 15% minimum brightness */
    float brt = get_brightness_factor() * (0.15f + 0.85f * val);

    struct color_rgb_float out = {rgb.r * brt, rgb.g * brt, rgb.b * brt};
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i] = out;
    }

    /* Advance phase according to animation speed */
    breathing_phase += anim_speed() * 0.003f;
    if (breathing_phase >= 1.0f)
        breathing_phase -= 1.0f;
}

static void breathing_reset(void) {
    breathing_phase = 0.0f;
}
