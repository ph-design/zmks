/* SWIRL effect (phase-shifted wave) — uses physical X position */
static float swirl_offset = 0.0f;
static void zmk_rgb_underglow_effect_swirl(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        float phase = led_norm_x(i); /* 0-1 normalised X */
        float angle = phase * 4.0f * M_PI + swirl_offset;
        float v = (sinf(angle) + 1.0f) / 2.0f;
        struct color_hsl step = hsl;
        struct color_rgb_float rgb;
        hsl_to_rgb_float(&step, &rgb);
        rgb.r *= brt * (0.15f + 0.85f * v);
        rgb.g *= brt * (0.15f + 0.85f * v);
        rgb.b *= brt * (0.15f + 0.85f * v);
        fx_pixels[i] = rgb;
    }

    swirl_offset += anim_speed() * 0.06f;
    if (swirl_offset > 2.0f * M_PI)
        swirl_offset -= 2.0f * M_PI;
}

static void swirl_reset(void) { swirl_offset = 0.0f; }
