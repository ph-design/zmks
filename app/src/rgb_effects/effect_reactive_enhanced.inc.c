/* REACTIVE ENHANCED: variant of reactive with larger fade and color interpolation */
static void zmk_rgb_underglow_effect_reactive_enhanced(void) {
    /* Use same color as base reactive but apply stronger interpolation */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t b = reactive_brightness[i];
        if (b == 0) {
            fx_pixels[i] = (struct color_rgb_float){0, 0, 0};
            continue;
        }

        struct color_hsl hsl = hsb_to_hsl(state.color);
        struct color_rgb_float rgb;
        hsl_to_rgb_float(&hsl, &rgb);

        float factor = (float)b / 255.0f;
        /* make fade longer by squaring factor for smoother tail */
        factor = factor * factor;

        rgb.r *= get_brightness_factor() * factor;
        rgb.g *= get_brightness_factor() * factor;
        rgb.b *= get_brightness_factor() * factor;

        fx_pixels[i] = rgb;
        /* decay */
        if (reactive_brightness[i] > 3)
            reactive_brightness[i] -= 3;
        else
            reactive_brightness[i] = 0;
    }
}

static void reactive_enhanced_reset(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++)
        reactive_brightness[i] = 0;
}
