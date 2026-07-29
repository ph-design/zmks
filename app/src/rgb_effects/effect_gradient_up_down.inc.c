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

static void gradient_up_down_reset(void) { gradient_ud_offset = 0.0f; }

static void zmk_rgb_underglow_effect_gradient_up_down(void) {
    float brt = get_brightness_factor();
    uint8_t sat = state.color.s;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        /* Use normalised Y position (0-1) for vertical gradient */
        float row_norm = led_norm_y(i);

        /* Spread 360 degrees across vertical span */
        float hue = hue_wrap((float)state.color.h + gradient_ud_offset + row_norm * 360.0f);

        struct color_rgb_float rgb;
        hsv_to_rgb_float((uint16_t)hue, sat, 100, &rgb);

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
    gradient_ud_offset += 0.5f + anim_speed() * 0.5f;
    if (gradient_ud_offset >= 360.0f)
        gradient_ud_offset -= 360.0f;
}
