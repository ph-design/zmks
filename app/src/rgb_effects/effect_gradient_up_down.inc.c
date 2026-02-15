/* GRADIENT UP-DOWN: Full-spectrum vertical gradient that scrolls over time.
 *
 * Each row of the keyboard gets a different hue, spanning the full 360°
 * spectrum from top to bottom.  The gradient scrolls continuously.
 *
 * Controls:
 *   H  – base hue offset (shifts the whole gradient)
 *   S  – saturation
 *   B  – brightness
 *   Speed – scroll rate (1 = slow drift, 5 = rapid cycle)
 *
 * QMK equivalent: GRADIENT_UP_DOWN
 */

static float gradient_ud_offset = 0.0f;

/* Grid helpers (local to this effect) */
static int gud_cols(void) {
    if (STRIP_NUM_PIXELS >= 120) return STRIP_NUM_PIXELS / 5;
    if (STRIP_NUM_PIXELS >= 48)  return STRIP_NUM_PIXELS / 4;
    if (STRIP_NUM_PIXELS >= 30)  return STRIP_NUM_PIXELS / 3;
    return STRIP_NUM_PIXELS;
}

static void gradient_up_down_reset(void) {
    gradient_ud_offset = 0.0f;
}

static void zmk_rgb_underglow_effect_gradient_up_down(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();

    int cols = gud_cols();
    int rows = (cols > 0) ? ((STRIP_NUM_PIXELS + cols - 1) / cols) : 1;
    if (rows < 1) rows = 1;

    /* Pre-compute one RGB per row (avoid redundant HSL→RGB for every pixel) */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int row = i / cols;

        /* Spread 360° across the rows, offset by base hue and scroll */
        float hue = hue_wrap(
            (float)state.color.h +
            gradient_ud_offset +
            (float)row / (float)rows * 360.0f);

        struct color_hsl pixel_hsl = {(uint16_t)hue, hsl.s, hsl.l};
        struct color_rgb_float rgb;
        hsl_to_rgb_float(&pixel_hsl, &rgb);

        fx_pixels[i] = (struct color_rgb_float){
            .r = rgb.r * brt,
            .g = rgb.g * brt,
            .b = rgb.b * brt,
        };
    }

    /* Advance scroll offset.
     * Speed 1 → 1.0°/frame → full cycle in 6 s @ 60 FPS
     * Speed 3 → 2.0°/frame → full cycle in 3 s
     * Speed 5 → 3.0°/frame → full cycle in 2 s
     */
    gradient_ud_offset += 0.5f + (float)state.animation_speed * 0.5f;
    if (gradient_ud_offset >= 360.0f)
        gradient_ud_offset -= 360.0f;
}
