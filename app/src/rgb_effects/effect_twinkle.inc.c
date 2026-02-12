/* TWINKLE / GLITTER effect (sparser sparkle) */
static void zmk_rgb_underglow_effect_twinkle(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        if ((rand() % 100) < (5 * state.animation_speed)) {
            struct color_rgb_float rgb;
            hsl_to_rgb_float(&hsl, &rgb);
            rgb.r *= brt;
            rgb.g *= brt;
            rgb.b *= brt;
            fx_pixels[i] = rgb;
        } else {
            /* decay previous pixel smoothly */
            struct color_rgb_float cur = fx_pixels[i];
            cur.r *= 0.9f;
            cur.g *= 0.9f;
            cur.b *= 0.9f;
            fx_pixels[i] = cur;
        }
    }
}

static void twinkle_reset(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++)
        fx_pixels[i] = (struct color_rgb_float){0, 0, 0};
}
