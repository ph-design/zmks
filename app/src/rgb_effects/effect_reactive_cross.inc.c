/* REACTIVE CROSS effect: full row + column illuminate on keypress, then fade.
 *
 * Unlike REACTIVE_NEXUS (which has expanding arms), CROSS illuminates the
 * entire row and column of the pressed key instantly, then fades out.
 * Intensity decreases with distance from the key for a natural falloff.
 *
 * Controls:
 *   H  – base hue (shifts slightly as the cross ages)
 *   S  – saturation
 *   B  – brightness
 *   Speed – fade rate (1 = slow fade, 5 = fast snap)
 *
 * QMK equivalent: SOLID_REACTIVE_MULTICROSS
 */

#define REACTIVE_CROSS_MAX_EVENTS 16

struct reactive_cross_event {
    int col;         /* column of keypress */
    int row;         /* row of keypress */
    uint8_t age;     /* frames since press */
    bool active;
};

static struct reactive_cross_event rc_events[REACTIVE_CROSS_MAX_EVENTS];

/* Grid helpers (local) */
static int rc_cols(void) {
    if (STRIP_NUM_PIXELS >= 120) return STRIP_NUM_PIXELS / 5;
    if (STRIP_NUM_PIXELS >= 48)  return STRIP_NUM_PIXELS / 4;
    if (STRIP_NUM_PIXELS >= 30)  return STRIP_NUM_PIXELS / 3;
    return STRIP_NUM_PIXELS;
}

static int rc_rows(void) {
    int c = rc_cols();
    return (c > 0) ? ((STRIP_NUM_PIXELS + c - 1) / c) : 1;
}

static void reactive_cross_add_event(uint32_t position) {
    if (position >= STRIP_NUM_PIXELS)
        return;
    int cols = rc_cols();
    int col = (int)position % cols;
    int row = (int)position / cols;

    /* Find free slot or recycle oldest */
    int oldest = 0;
    uint8_t oldest_age = 0;
    for (int i = 0; i < REACTIVE_CROSS_MAX_EVENTS; i++) {
        if (!rc_events[i].active) {
            rc_events[i].col = col;
            rc_events[i].row = row;
            rc_events[i].age = 0;
            rc_events[i].active = true;
            return;
        }
        if (rc_events[i].age > oldest_age) {
            oldest_age = rc_events[i].age;
            oldest = i;
        }
    }
    rc_events[oldest].col = col;
    rc_events[oldest].row = row;
    rc_events[oldest].age = 0;
    rc_events[oldest].active = true;
}

static void reactive_cross_reset(void) {
    for (int i = 0; i < REACTIVE_CROSS_MAX_EVENTS; i++)
        rc_events[i].active = false;
}

static void zmk_rgb_underglow_effect_reactive_cross(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();

    /* Max lifetime: Speed 1 → 80 fr (1.3 s), Speed 5 → 16 fr (0.27 s) */
    uint8_t max_age = 80 / (state.animation_speed > 0 ? state.animation_speed : 1);
    if (max_age < 8)
        max_age = 8;

    int cols = rc_cols();
    int rows = rc_rows();

    /* Clear canvas */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i] = (struct color_rgb_float){0, 0, 0};
    }

    for (int e = 0; e < REACTIVE_CROSS_MAX_EVENTS; e++) {
        if (!rc_events[e].active)
            continue;

        float age_norm = (float)rc_events[e].age / (float)max_age;
        if (age_norm >= 1.0f) {
            rc_events[e].active = false;
            continue;
        }

        /* Quadratic fade for smooth tail */
        float fade = 1.0f - age_norm;
        fade *= fade;

        /* Slight hue shift as cross ages (up to 25°) */
        uint16_t hue = (hsl.h + (uint16_t)((1.0f - fade) * 25)) % 360;
        struct color_hsl shifted = {hue, hsl.s, hsl.l};
        struct color_rgb_float rgb;
        hsl_to_rgb_float(&shifted, &rgb);

        for (int j = 0; j < STRIP_NUM_PIXELS; j++) {
            int jcol = j % cols;
            int jrow = j / cols;

            /* Pixel is on the cross only if it shares the row OR column */
            bool on_row = (jrow == rc_events[e].row);
            bool on_col = (jcol == rc_events[e].col);
            if (!on_row && !on_col)
                continue;

            /* Spatial falloff: pixels further from center are dimmer.
             * Use the perpendicular distance (along the arm).
             */
            float spatial;
            if (on_row && on_col) {
                /* Intersection point — full brightness */
                spatial = 1.0f;
            } else if (on_row) {
                /* Horizontal arm: falloff along column distance */
                int dcol = abs(jcol - rc_events[e].col);
                spatial = 1.0f - (float)dcol / (float)(cols > 1 ? cols : 1);
            } else {
                /* Vertical arm: falloff along row distance */
                int drow = abs(jrow - rc_events[e].row);
                spatial = 1.0f - (float)drow / (float)(rows > 1 ? rows : 1);
            }
            if (spatial <= 0.0f)
                continue;

            /* Smooth the spatial falloff */
            spatial *= spatial;

            float intensity = fade * spatial * brt;
            struct color_rgb_float c = {
                .r = rgb.r * intensity,
                .g = rgb.g * intensity,
                .b = rgb.b * intensity,
            };

            /* Lighten blend */
            if (c.r > fx_pixels[j].r) fx_pixels[j].r = c.r;
            if (c.g > fx_pixels[j].g) fx_pixels[j].g = c.g;
            if (c.b > fx_pixels[j].b) fx_pixels[j].b = c.b;
        }

        rc_events[e].age++;
    }
}
