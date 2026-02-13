/* RAINBOW / HUE CYCLE effect */
static float rainbow_offset = 0.0f;
static void zmk_rgb_underglow_effect_rainbow(void) {
    struct color_hsl base = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();
    float hue_span = 360.0f / (float)STRIP_NUM_PIXELS;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        /* Incremental hue stepping avoids fmodf per pixel */
        float hue = (float)base.h + rainbow_offset + hue_span * (float)i;
        /* Fast modulo for positive values */
        while (hue >= 360.0f) hue -= 360.0f;
        while (hue < 0.0f) hue += 360.0f;

        struct color_hsl hsl = {(uint16_t)hue, base.s, base.l};
        struct color_rgb_float rgb;
        hsl_to_rgb_float(&hsl, &rgb);
        fx_pixels[i] = (struct color_rgb_float){
            .r = rgb.r * brt,
            .g = rgb.g * brt,
            .b = rgb.b * brt
        };
    }

    rainbow_offset += (float)state.animation_speed * 1.8f;
    if (rainbow_offset >= 360.0f)
        rainbow_offset -= 360.0f;
}

static void rainbow_reset(void) {
    rainbow_offset = 0.0f;
}
