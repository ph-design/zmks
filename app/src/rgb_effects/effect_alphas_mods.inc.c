/* ALPHAS MODS effect: static dual hue — modifier keys use a secondary hue,
 * alpha keys use the user-selected hue.  The secondary hue offset is
 * controlled by animation_speed (higher speed → larger hue gap).
 *
 * Uses physical coordinates when available: LEDs near the left/right edges
 * (X < 0.12 or X > 0.88) are treated as modifiers.  Falls back to
 * index-based heuristic when coordinates are unavailable.
 *
 * Inspired by QMK RGB_MATRIX_ALPHAS_MODS.
 */

static void zmk_rgb_underglow_effect_alphas_mods(void) {
    uint8_t sat = state.color.s;
    /* Secondary hue: offset by speed × 30 degrees (speed 1→30°, 5→150°) */
    uint16_t hue_alpha = state.color.h;
    uint16_t hue_mod = (hue_alpha + (uint16_t)anim_speed() * 30) % 360;

    struct color_rgb_float rgb_alpha, rgb_mod;
    hsv_to_rgb_float(hue_alpha, sat, 100, &rgb_alpha);
    hsv_to_rgb_float(hue_mod, sat, 100, &rgb_mod);

    float brt = get_brightness_factor();
    rgb_alpha.r *= brt;
    rgb_alpha.g *= brt;
    rgb_alpha.b *= brt;
    rgb_mod.r *= brt;
    rgb_mod.g *= brt;
    rgb_mod.b *= brt;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        float x = led_norm_x(i);
        /* Edge keys (left/right ~12%) → "modifier" */
        bool is_mod = (x < 0.12f) || (x > 0.88f);
        fx_pixels[i] = is_mod ? rgb_mod : rgb_alpha;
    }
}

static void alphas_mods_reset(void) { /* Static effect — nothing to reset */ }
