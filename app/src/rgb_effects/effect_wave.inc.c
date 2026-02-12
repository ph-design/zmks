/* WAVE effect */
static float wave_offset = 0.0f;
static void zmk_rgb_underglow_effect_wave(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int key_pos = effect_pixel_lookup(i);
        float phase = (float)key_pos / (float)STRIP_NUM_PIXELS;
        float v = (sinf((phase * 2.0f * M_PI) + wave_offset) + 1.0f) / 2.0f;
        struct color_hsl step = hsl;
        struct color_rgb_float rgb;
        hsl_to_rgb_float(&step, &rgb);
        rgb.r *= brt * (0.2f + 0.8f * v);
        rgb.g *= brt * (0.2f + 0.8f * v);
        rgb.b *= brt * (0.2f + 0.8f * v);
        fx_pixels[i] = rgb;
    }

    wave_offset += (float)state.animation_speed * 0.05f;
    if (wave_offset > 2.0f * M_PI)
        wave_offset -= 2.0f * M_PI;
}

static void wave_reset(void) {
    wave_offset = 0.0f;
}
