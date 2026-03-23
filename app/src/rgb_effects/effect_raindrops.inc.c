/* RAINDROPS effect: randomly changes a single key's hue each frame.
 *
 * Inspired by QMK RGB_MATRIX_RAINDROPS / RGB_MATRIX_JELLYBEAN_RAINDROPS.
 *
 * Each LED starts with the user-selected hue and over time random LEDs
 * are reassigned a completely random hue (jellybean variant — more
 * colourful).  The number of LEDs changed per frame scales with
 * animation_speed.
 */

/* Per-pixel hue storage; initialised on reset */
static uint16_t raindrops_hue[STRIP_NUM_PIXELS];

static void zmk_rgb_underglow_effect_raindrops(void) {
    struct color_hsl base = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();

    /* Pick 1..speed random LEDs per frame and assign random hue */
    int changes = (int)anim_speed();
    if (changes < 1) changes = 1;
    for (int c = 0; c < changes; c++) {
        int idx = rand() % STRIP_NUM_PIXELS;
        raindrops_hue[idx] = (uint16_t)(rand() % 360);
    }

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct color_hsl hsl = {raindrops_hue[i], base.s, base.l};
        struct color_rgb_float rgb;
        hsl_to_rgb_float(&hsl, &rgb);
        rgb.r *= brt;
        rgb.g *= brt;
        rgb.b *= brt;
        fx_pixels[i] = rgb;
    }
}

static void raindrops_reset(void) {
    struct color_hsl base = hsb_to_hsl(state.color);
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        raindrops_hue[i] = base.h;
    }
}
