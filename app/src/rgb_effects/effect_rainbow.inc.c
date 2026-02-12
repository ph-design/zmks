/* RAINBOW / HUE CYCLE effect */
static float rainbow_offset = 0.0f;
static void zmk_rgb_underglow_effect_rainbow(void) {
    struct color_hsl base = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        /* Spread hues across strip and add offset for animation */
        float hue = fmodf((float)base.h + rainbow_offset + ((float)i / (float)STRIP_NUM_PIXELS) * 360.0f, 360.0f);
        struct color_hsl hsl = {(uint16_t)hue, base.s, base.l};
        struct color_rgb_float rgb;
        hsl_to_rgb_float(&hsl, &rgb);
        rgb.r *= brt;
        rgb.g *= brt;
        rgb.b *= brt;
        fx_pixels[i] = rgb;
    }

    rainbow_offset += (float)state.animation_speed * 1.8f;
    if (rainbow_offset >= 360.0f)
        rainbow_offset -= 360.0f;
}

static void rainbow_reset(void) {
    rainbow_offset = 0.0f;
}
