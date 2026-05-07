/* WAVE effect — uses physical X coordinate for wave phase */
static float wave_offset = 0.0f;
static void zmk_rgb_underglow_effect_wave(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float base_rgb;
    hsl_to_rgb_float(&hsl, &base_rgb);
    float brt = get_brightness_factor();

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        float phase = led_norm_x(i); /* 0-1 normalised physical X */
        /* Fast sine approximation: parabolic half-wave */
        float x = phase + wave_offset * (1.0f / (2.0f * M_PI));
        x = x - (int)x; /* fract */
        if (x < 0)
            x += 1.0f;
        float v;
        if (x < 0.5f) {
            float t = x * 2.0f;
            v = 4.0f * t * (1.0f - t);
        } else {
            float t = (x - 0.5f) * 2.0f;
            v = -4.0f * t * (1.0f - t);
        }
        v = (v + 1.0f) * 0.5f; /* normalize to 0..1 */
        float f = brt * (0.15f + 0.85f * v);
        fx_pixels[i] =
            (struct color_rgb_float){.r = base_rgb.r * f, .g = base_rgb.g * f, .b = base_rgb.b * f};
    }

    wave_offset += anim_speed() * 0.025f;
    if (wave_offset > 2.0f * M_PI)
        wave_offset -= 2.0f * M_PI;
}

static void wave_reset(void) { wave_offset = 0.0f; }
