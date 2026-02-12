/* SOLID effect (moved out of rgb_underglow.c to reduce file size) */
/* Maintains original static linkage when included into rgb_underglow.c */
static void zmk_rgb_underglow_effect_solid(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float rgb;
    float brt = get_brightness_factor();

    if (state.animation_speed > 1) {
        /* Cycle through complementary hue with HSL interpolation */
        uint16_t cycle_duration = ANIMATION_FPS * 10 / state.animation_speed;
        if (cycle_duration < 1)
            cycle_duration = 1;

        struct color_hsl hsl2 = hsl;
        hsl2.h = (hsl.h + 180) % 360;

        struct color_hsl interp;
        float step = (float)(solid_counter % cycle_duration) / (float)cycle_duration;
        /* Triangle wave: 0 -> 1 -> 0 for smooth back-and-forth */
        if (step > 0.5f)
            step = 1.0f - step;
        step *= 2.0f;
        interpolate_hsl(&hsl, &hsl2, &interp, step);
        hsl_to_rgb_float(&interp, &rgb);

        solid_counter++;
        if (solid_counter >= cycle_duration)
            solid_counter = 0;
    } else {
        hsl_to_rgb_float(&hsl, &rgb);
    }

    rgb.r *= brt;
    rgb.g *= brt;
    rgb.b *= brt;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i] = rgb;
    }
}

static void solid_reset(void) {
    solid_counter = 0;
}
