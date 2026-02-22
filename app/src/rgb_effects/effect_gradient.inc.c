/* LINEAR GRADIENT effect — uses physical X coordinate for position */
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
    struct color_rgb_float colors[3] = {rgb1, rgb2, rgb3};

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        /* Use normalised X position (0-1) mapped to gradient space */
        float pos = led_norm_x(i); /* 0-1 */
        float distance = float_mod(1.0f + pos - gradient_offset, 1.0f);
        if (distance < 0)
            distance += 1.0f;

        float segment_width = 1.0f / 3.0f;
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

    /* Scroll (normalised 0-1 range) */
    gradient_offset += (float)state.animation_speed * 0.002f;
    if (gradient_offset >= 1.0f) {
        gradient_offset -= 1.0f;
    }
}

static void gradient_reset(void) { gradient_offset = 0.0f; }
