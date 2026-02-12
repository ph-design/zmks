/* LINEAR GRADIENT effect (moved out of rgb_underglow.c) */
static void zmk_rgb_underglow_effect_gradient(void) {
    struct color_hsl hsl1 = hsb_to_hsl(state.color);
    struct color_hsl hsl2 = hsl1;
    hsl2.h = (hsl1.h + 120) % 360;
    struct color_hsl hsl3 = hsl1;
    hsl3.h = (hsl1.h + 240) % 360;

    struct color_rgb_float rgb1, rgb2, rgb3;
    hsl_to_rgb_float(&hsl1, &rgb1);
    hsl_to_rgb_float(&hsl2, &rgb2);
    hsl_to_rgb_float(&hsl3, &rgb3);

    float brt = get_brightness_factor();
    float gradient_width = (float)STRIP_NUM_PIXELS;
    struct color_rgb_float colors[3] = {rgb1, rgb2, rgb3};

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        /* Map LED strip index to physical key position for smooth gradient */
        int key_pos = effect_pixel_lookup(i);
        float distance = float_mod(gradient_width + (float)key_pos - gradient_offset, gradient_width);
        if (distance < 0)
            distance += gradient_width;

        float segment_width = gradient_width / 3.0f;
        int from_idx = (int)(distance / segment_width);
        if (from_idx > 2)
            from_idx = 2;
        float step = (distance - from_idx * segment_width) / segment_width;
        if (step > 1.0f)
            step = 1.0f;

        struct color_rgb_float result;
        interpolate_rgb(&colors[from_idx], &colors[(from_idx + 1) % 3], &result, step);

        result.r *= brt;
        result.g *= brt;
        result.b *= brt;
        fx_pixels[i] = result;
    }

    /* Scroll */
    gradient_offset += (float)state.animation_speed * 0.15f;
    if (gradient_offset >= gradient_width) {
        gradient_offset -= gradient_width;
    }
}

static void gradient_reset(void) {
    gradient_offset = 0.0f;
}
