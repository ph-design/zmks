/* LINEAR GRADIENT effect — uses physical X coordinate for position */
static void zmk_rgb_underglow_effect_gradient(void) {
    uint16_t h1 = state.color.h;
    uint16_t h2 = (h1 + 120) % 360;
    uint16_t h3 = (h1 + 240) % 360;
    uint8_t sat = state.color.s;

    struct color_rgb_float rgb1, rgb2, rgb3;
    hsv_to_rgb_float(h1, sat, 100, &rgb1);
    hsv_to_rgb_float(h2, sat, 100, &rgb2);
    hsv_to_rgb_float(h3, sat, 100, &rgb3);

    float brt = get_brightness_factor();
    struct color_rgb_float colors[3] = {rgb1, rgb2, rgb3};

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        /* Use normalised X position (0-1) mapped to gradient space */
        float pos = led_norm_x(i); /* 0-1 */
        float distance = fmodf(1.0f + pos - gradient_offset, 1.0f);

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
    gradient_offset += anim_speed() * 0.002f;
    if (gradient_offset >= 1.0f) {
        gradient_offset -= 1.0f;
    }
}

static void gradient_reset(void) { gradient_offset = 0.0f; }
