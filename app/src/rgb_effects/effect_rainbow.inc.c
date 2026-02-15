/* RAINBOW effect variants
 *
 * All variants share the same state/reset but distribute hue differently:
 *   RAINBOW              — left-to-right linear hue sweep (CYCLE_LEFT_RIGHT)
 *   RAINBOW_ALL          — all keys cycle hue together     (CYCLE_ALL)
 *   RAINBOW_UP_DOWN      — top-to-bottom hue sweep         (CYCLE_UP_DOWN)
 *   RAINBOW_PINWHEEL     — angular hue sweep around center (CYCLE_PINWHEEL)
 *   RAINBOW_OUT_IN       — radial hue sweep from center    (CYCLE_OUT_IN)
 *   RAINBOW_CHEVRON      — V-shape moving hue pattern      (RAINBOW_MOVING_CHEVRON)
 *   RAINBOW_DUAL_BEACON  — two opposite rotating beacons   (DUAL_BEACON)
 */

static float rainbow_offset = 0.0f;

/* ── Approximate row/col layout helpers (shared across variants) ──────── */
static int rb_cols(void) {
    if (STRIP_NUM_PIXELS >= 120)
        return STRIP_NUM_PIXELS / 5;
    if (STRIP_NUM_PIXELS >= 48)
        return STRIP_NUM_PIXELS / 4;
    if (STRIP_NUM_PIXELS >= 30)
        return STRIP_NUM_PIXELS / 3;
    return STRIP_NUM_PIXELS;
}
static int rb_rows(void) {
    int c = rb_cols();
    return (c > 0) ? ((STRIP_NUM_PIXELS + c - 1) / c) : 1;
}

/* ── Helper: wrap hue into [0, 360) ──────────────────────────────────── */
static inline float hue_wrap(float h) {
    while (h >= 360.0f)
        h -= 360.0f;
    while (h < 0.0f)
        h += 360.0f;
    return h;
}

/* ── Helper: emit one pixel at a given hue ───────────────────────────── */
static inline void rainbow_set_pixel(int idx, float hue, uint8_t s, uint8_t l, float brt) {
    struct color_hsl hsl = {(uint16_t)hue_wrap(hue), s, l};
    struct color_rgb_float rgb;
    hsl_to_rgb_float(&hsl, &rgb);
    fx_pixels[idx] = (struct color_rgb_float){.r = rgb.r * brt, .g = rgb.g * brt, .b = rgb.b * brt};
}

/* ───────────────────────────────────────────────────────────────────────
 * 1. RAINBOW (left-to-right) — original, equivalent to QMK CYCLE_LEFT_RIGHT
 * ─────────────────────────────────────────────────────────────────────── */
static void zmk_rgb_underglow_effect_rainbow(void) {
    struct color_hsl base = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();
    float hue_span = 360.0f / (float)STRIP_NUM_PIXELS;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        float hue = (float)base.h + rainbow_offset + hue_span * (float)i;
        rainbow_set_pixel(i, hue, base.s, base.l, brt);
    }

    rainbow_offset += (float)state.animation_speed * 1.8f;
    if (rainbow_offset >= 360.0f)
        rainbow_offset -= 360.0f;
}

#if 0 /* DISABLED: Rainbow All — re-enable by adding to enum + effect_table */
static void zmk_rgb_underglow_effect_rainbow_all(void) {
    struct color_hsl base = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();
    float hue = (float)base.h + rainbow_offset;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        rainbow_set_pixel(i, hue, base.s, base.l, brt);
    }

    rainbow_offset += (float)state.animation_speed * 1.8f;
    if (rainbow_offset >= 360.0f)
        rainbow_offset -= 360.0f;
}
#endif

/* ───────────────────────────────────────────────────────────────────────
 * 3. RAINBOW UP-DOWN — hue changes along rows (QMK CYCLE_UP_DOWN)
 * ─────────────────────────────────────────────────────────────────────── */
static void zmk_rgb_underglow_effect_rainbow_up_down(void) {
    struct color_hsl base = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();
    int cols = rb_cols();
    int rows = rb_rows();
    float hue_span = (rows > 1) ? 360.0f / (float)rows : 0.0f;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int row = (cols > 0) ? (i / cols) : 0;
        float hue = (float)base.h + rainbow_offset + hue_span * (float)row;
        rainbow_set_pixel(i, hue, base.s, base.l, brt);
    }

    rainbow_offset += (float)state.animation_speed * 1.8f;
    if (rainbow_offset >= 360.0f)
        rainbow_offset -= 360.0f;
}

/* ───────────────────────────────────────────────────────────────────────
 * 4. RAINBOW PINWHEEL — angular/rotational hue (QMK CYCLE_PINWHEEL)
 * ─────────────────────────────────────────────────────────────────────── */
static void zmk_rgb_underglow_effect_rainbow_pinwheel(void) {
    struct color_hsl base = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();
    int cols = rb_cols();
    int rows = rb_rows();
    float cx = (float)(cols - 1) / 2.0f;
    float cy = (float)(rows - 1) / 2.0f;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int col = (cols > 0) ? (i % cols) : 0;
        int row = (cols > 0) ? (i / cols) : 0;
        /* atan2 gives angle in radians; map to 0-360 degrees */
        float angle = atan2f((float)row - cy, (float)col - cx);
        float hue = (float)base.h + rainbow_offset + angle * (180.0f / M_PI);
        rainbow_set_pixel(i, hue, base.s, base.l, brt);
    }

    rainbow_offset += (float)state.animation_speed * 1.8f;
    if (rainbow_offset >= 360.0f)
        rainbow_offset -= 360.0f;
}

/* ───────────────────────────────────────────────────────────────────────
 * 5. RAINBOW OUT-IN — radial distance from center (QMK CYCLE_OUT_IN)
 * ─────────────────────────────────────────────────────────────────────── */
static void zmk_rgb_underglow_effect_rainbow_out_in(void) {
    struct color_hsl base = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();
    int cols = rb_cols();
    int rows = rb_rows();
    float cx = (float)(cols - 1) / 2.0f;
    float cy = (float)(rows - 1) / 2.0f;
    /* Max possible distance for normalization */
    float max_dist = sqrtf(cx * cx + cy * cy);
    if (max_dist < 1.0f)
        max_dist = 1.0f;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int col = (cols > 0) ? (i % cols) : 0;
        int row = (cols > 0) ? (i / cols) : 0;
        float dx = (float)col - cx;
        float dy = (float)row - cy;
        float dist = sqrtf(dx * dx + dy * dy) / max_dist;
        float hue = (float)base.h + rainbow_offset + dist * 360.0f;
        rainbow_set_pixel(i, hue, base.s, base.l, brt);
    }

    rainbow_offset += (float)state.animation_speed * 1.8f;
    if (rainbow_offset >= 360.0f)
        rainbow_offset -= 360.0f;
}

#if 0 /* DISABLED: Rainbow Chevron — re-enable by adding to enum + effect_table */
static void zmk_rgb_underglow_effect_rainbow_chevron(void) {
    struct color_hsl base = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();
    int cols = rb_cols();
    int rows = rb_rows();
    float cy = (float)(rows - 1) / 2.0f;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int col = (cols > 0) ? (i % cols) : 0;
        int row = (cols > 0) ? (i / cols) : 0;
        float v_dist = fabsf((float)row - cy);
        float norm = ((float)col + v_dist * 1.5f) / ((float)cols + (float)rows);
        float hue = (float)base.h + rainbow_offset + norm * 360.0f;
        rainbow_set_pixel(i, hue, base.s, base.l, brt);
    }

    rainbow_offset += (float)state.animation_speed * 1.8f;
    if (rainbow_offset >= 360.0f)
        rainbow_offset -= 360.0f;
}
#endif

#if 0 /* DISABLED: Rainbow Dual Beacon — re-enable by adding to enum + effect_table */
static void zmk_rgb_underglow_effect_rainbow_dual_beacon(void) {
    struct color_hsl base = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();
    int cols = rb_cols();
    int rows = rb_rows();
    float cx = (float)(cols - 1) / 2.0f;
    float cy = (float)(rows - 1) / 2.0f;
    float beam_angle = rainbow_offset * (M_PI / 180.0f); /* convert to radians */

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int col = (cols > 0) ? (i % cols) : 0;
        int row = (cols > 0) ? (i / cols) : 0;
        float angle = atan2f((float)row - cy, (float)col - cx);
        /* Two beams 180° apart */
        float diff1 = angle - beam_angle;
        float diff2 = angle - beam_angle - M_PI;
        /* Wrap to [-PI, PI] */
        while (diff1 > M_PI)
            diff1 -= 2.0f * M_PI;
        while (diff1 < -M_PI)
            diff1 += 2.0f * M_PI;
        while (diff2 > M_PI)
            diff2 -= 2.0f * M_PI;
        while (diff2 < -M_PI)
            diff2 += 2.0f * M_PI;
        float d1 = fabsf(diff1) / M_PI;
        float d2 = fabsf(diff2) / M_PI;
        float proximity = 1.0f - (d1 < d2 ? d1 : d2);
        /* Brightness falls off with angular distance */
        float f = brt * (0.05f + 0.95f * proximity * proximity);

        struct color_hsl hsl = {base.h, base.s, base.l};
        struct color_rgb_float rgb;
        hsl_to_rgb_float(&hsl, &rgb);
        fx_pixels[i] = (struct color_rgb_float){.r = rgb.r * f, .g = rgb.g * f, .b = rgb.b * f};
    }

    rainbow_offset += (float)state.animation_speed * 2.5f;
    if (rainbow_offset >= 360.0f)
        rainbow_offset -= 360.0f;
}
#endif

/* ── Shared reset ────────────────────────────────────────────────────── */
static void rainbow_reset(void) { rainbow_offset = 0.0f; }
