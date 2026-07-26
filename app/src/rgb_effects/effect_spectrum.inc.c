static float spectrum_hue = 0.0f;

static void zmk_rgb_underglow_effect_spectrum(void) {
    struct color_hsl base = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();
    struct color_hsl hsl = {(uint16_t)spectrum_hue, base.s, base.l};
    struct color_rgb_float rgb;
    hsl_to_rgb_float(&hsl, &rgb);

    struct color_rgb_float out = {rgb.r * brt, rgb.g * brt, rgb.b * brt};
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i] = out;
    }

    spectrum_hue += anim_speed() * 1.8f;
    if (spectrum_hue >= 360.0f)
        spectrum_hue -= 360.0f;
}

static void spectrum_reset(void) { spectrum_hue = 0.0f; }
