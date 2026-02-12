/* STROBE effect (periodic flash) */
static int strobe_counter = 0;
static void zmk_rgb_underglow_effect_strobe(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float rgb;
    hsl_to_rgb_float(&hsl, &rgb);
    float brt = get_brightness_factor();

    int period = MAX(1, 10 - state.animation_speed * 2);
    bool on = (strobe_counter % period) == 0;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct color_rgb_float out = {0, 0, 0};
        if (on) {
            out = rgb;
            out.r *= brt;
            out.g *= brt;
            out.b *= brt;
        }
        fx_pixels[i] = out;
    }

    strobe_counter = (strobe_counter + 1) & 0xFFFF;
}

static void strobe_reset(void) {
    strobe_counter = 0;
}
