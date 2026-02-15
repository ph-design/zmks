/* ALPHAS MODS effect: static dual hue — modifier keys use a secondary hue,
 * alpha keys use the user-selected hue.  The secondary hue offset is
 * controlled by animation_speed (higher speed → larger hue gap).
 *
 * Inspired by QMK RGB_MATRIX_ALPHAS_MODS.
 *
 * Since ZMK lacks per-key flags, we use a simple heuristic: keys in the
 * first and last two physical positions of each "row-equivalent slice"
 * are treated as modifiers.  This gives a visually correct result on
 * most ortho/staggered boards without any extra configuration.
 */

static void zmk_rgb_underglow_effect_alphas_mods(void) {
    struct color_hsl hsl_alpha = hsb_to_hsl(state.color);
    struct color_hsl hsl_mod = hsl_alpha;
    /* Secondary hue: offset by speed × 30 degrees (speed 1→30°, 5→150°) */
    hsl_mod.h = (hsl_alpha.h + (uint16_t)state.animation_speed * 30) % 360;

    struct color_rgb_float rgb_alpha, rgb_mod;
    hsl_to_rgb_float(&hsl_alpha, &rgb_alpha);
    hsl_to_rgb_float(&hsl_mod, &rgb_mod);

    float brt = get_brightness_factor();
    rgb_alpha.r *= brt; rgb_alpha.g *= brt; rgb_alpha.b *= brt;
    rgb_mod.r   *= brt; rgb_mod.g   *= brt; rgb_mod.b   *= brt;

    /* Approximate row length for modifier detection */
    int row_len = STRIP_NUM_PIXELS;
    if (row_len >= 120)      row_len = row_len / 5;  /* 5-row board */
    else if (row_len >= 48)  row_len = row_len / 4;  /* 4-row board */
    else if (row_len >= 30)  row_len = row_len / 3;  /* 3-row board */
    /* else treat entire strip as one row */

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int col_in_row = i % row_len;
        /* First 2 and last 2 columns → "modifier" */
        bool is_mod = (col_in_row < 2) || (col_in_row >= row_len - 2);
        fx_pixels[i] = is_mod ? rgb_mod : rgb_alpha;
    }
}

static void alphas_mods_reset(void) {
    /* Static effect — nothing to reset */
}
