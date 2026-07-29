/* SWIRL effect (phase-shifted wave) — uses physical X position */
static float swirl_offset = 0.0f;
static void zmk_rgb_underglow_effect_swirl(void) {
    struct color_rgb_float base_rgb;
    user_color_rgb_float(&base_rgb);
    float brt = get_brightness_factor();

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        float phase = led_norm_x(i); /* 0-1 normalised X */
        float angle = phase * 4.0f * M_PI + swirl_offset;
        float v = (sinf(angle) + 1.0f) / 2.0f;
        float f = brt * (0.15f + 0.85f * v);
        fx_pixels[i] = (struct color_rgb_float){
            .r = base_rgb.r * f,
            .g = base_rgb.g * f,
            .b = base_rgb.b * f,
        };
    }

    swirl_offset += anim_speed() * 0.06f;
    if (swirl_offset > 2.0f * M_PI)
        swirl_offset -= 2.0f * M_PI;
}

static void swirl_reset(void) { swirl_offset = 0.0f; }
