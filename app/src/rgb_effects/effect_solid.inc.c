/* SOLID effect — true static single colour, no animation */
static void zmk_rgb_underglow_effect_solid(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float rgb;
    hsl_to_rgb_float(&hsl, &rgb);
    float brt = get_brightness_factor();

    struct color_rgb_float out = {rgb.r * brt, rgb.g * brt, rgb.b * brt};
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i] = out;
    }
}

static void solid_reset(void) {
    solid_counter = 0;
}
