/* BREATHING effect */
static float breathing_phase = 0.0f;
static void zmk_rgb_underglow_effect_breathing(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float rgb;
    hsl_to_rgb_float(&hsl, &rgb);

    /* Smooth breathing using sine waveform */
    float phase = breathing_phase;
    float val = (sinf(phase * 2.0f * M_PI) + 1.0f) / 2.0f; /* 0..1 */
    float brt = get_brightness_factor() * (0.2f + 0.8f * val);

    rgb.r *= brt;
    rgb.g *= brt;
    rgb.b *= brt;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i] = rgb;
    }

    /* Advance phase according to animation speed */
    breathing_phase += (float)state.animation_speed * 0.0025f;
    if (breathing_phase >= 1.0f)
        breathing_phase -= 1.0f;
}

static void breathing_reset(void) {
    breathing_phase = 0.0f;
}
