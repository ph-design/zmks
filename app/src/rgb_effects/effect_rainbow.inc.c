/* RAINBOW / HUE CYCLE effect — uses physical X coordinate for hue spread */
static float rainbow_offset = 0.0f;
static void zmk_rgb_underglow_effect_rainbow(void) {
    float brt = get_brightness_factor();
    uint8_t sat = state.color.s;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        /* Use normalised X position (0-1) for a full 360° hue sweep */
        float hue = (float)state.color.h + rainbow_offset + led_norm_x(i) * 360.0f;
        /* Fast modulo for positive values */
        while (hue >= 360.0f)
            hue -= 360.0f;
        while (hue < 0.0f)
            hue += 360.0f;

        struct color_rgb_float rgb;
        hsv_to_rgb_float((uint16_t)hue, sat, 100, &rgb);
        fx_pixels[i] =
            (struct color_rgb_float){.r = rgb.r * brt, .g = rgb.g * brt, .b = rgb.b * brt};
    }

    rainbow_offset += anim_speed() * 1.4f;
    if (rainbow_offset >= 360.0f)
        rainbow_offset -= 360.0f;
}

static void rainbow_reset(void) { rainbow_offset = 0.0f; }
