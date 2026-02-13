/* REACTIVE NEXUS effect: hue & value pulse outward along the same column
 * and row of the pressed key, forming a cross/nexus pattern.
 *
 * Inspired by QMK RGB_MATRIX_SOLID_REACTIVE_MULTINEXUS.
 */

#define REACTIVE_NEXUS_MAX_EVENTS 16
#define REACTIVE_NEXUS_ARM_WIDTH  30  /* cross-arm width in 0-255 space */

struct reactive_nexus_event {
    int col;             /* column of source key (0-based) */
    int row;             /* row of source key (0-based) */
    uint8_t age;
    bool active;
};

static struct reactive_nexus_event rn_events[REACTIVE_NEXUS_MAX_EVENTS];

/* Approximate grid dimensions deduced from pixel count */
static int nexus_cols(void) {
    if (STRIP_NUM_PIXELS >= 120) return STRIP_NUM_PIXELS / 5;
    if (STRIP_NUM_PIXELS >= 48)  return STRIP_NUM_PIXELS / 4;
    if (STRIP_NUM_PIXELS >= 30)  return STRIP_NUM_PIXELS / 3;
    return STRIP_NUM_PIXELS;
}

static int nexus_rows(void) {
    int c = nexus_cols();
    return (c > 0) ? ((STRIP_NUM_PIXELS + c - 1) / c) : 1;
}

static void reactive_nexus_add_event(uint32_t position) {
    if (position >= STRIP_NUM_PIXELS)
        return;
    int cols = nexus_cols();
    int col = (int)position % cols;
    int row = (int)position / cols;

    int oldest = 0;
    uint8_t oldest_age = 0;
    for (int i = 0; i < REACTIVE_NEXUS_MAX_EVENTS; i++) {
        if (!rn_events[i].active) {
            rn_events[i].col = col;
            rn_events[i].row = row;
            rn_events[i].age = 0;
            rn_events[i].active = true;
            return;
        }
        if (rn_events[i].age > oldest_age) {
            oldest_age = rn_events[i].age;
            oldest = i;
        }
    }
    rn_events[oldest].col = col;
    rn_events[oldest].row = row;
    rn_events[oldest].age = 0;
    rn_events[oldest].active = true;
}

static void reactive_nexus_reset(void) {
    for (int i = 0; i < REACTIVE_NEXUS_MAX_EVENTS; i++)
        rn_events[i].active = false;
}

static void zmk_rgb_underglow_effect_reactive_nexus(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();

    uint8_t max_age = 100 / (state.animation_speed > 0 ? state.animation_speed : 1);
    if (max_age < 8) max_age = 8;

    int cols = nexus_cols();
    int rows = nexus_rows();

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i] = (struct color_rgb_float){0, 0, 0};
    }

    for (int e = 0; e < REACTIVE_NEXUS_MAX_EVENTS; e++) {
        if (!rn_events[e].active)
            continue;

        float age_factor = 1.0f - (float)rn_events[e].age / (float)max_age;
        if (age_factor <= 0) {
            rn_events[e].active = false;
            continue;
        }
        age_factor *= age_factor;

        struct color_hsl shifted = hsl;
        shifted.h = (hsl.h + (uint16_t)((1.0f - age_factor) * 40)) % 360;
        struct color_rgb_float rgb;
        hsl_to_rgb_float(&shifted, &rgb);

        for (int j = 0; j < STRIP_NUM_PIXELS; j++) {
            int key_pos = effect_pixel_lookup(j);
            int jcol = key_pos % cols;
            int jrow = key_pos / cols;

            /* Distance along the cross arms */
            int dcol = abs(jcol - rn_events[e].col);
            int drow = abs(jrow - rn_events[e].row);

            /* Nexus cross: contribute if on same row OR same column */
            float spatial = 0.0f;
            if (jrow == rn_events[e].row && cols > 0) {
                /* Horizontal arm: expand outward with age */
                float reach = (1.0f - age_factor) * (float)cols;
                if ((float)dcol <= reach + 1.0f) {
                    float arm = 1.0f - (float)dcol / (reach + 1.0f);
                    if (arm > spatial) spatial = arm;
                }
            }
            if (jcol == rn_events[e].col && rows > 0) {
                /* Vertical arm */
                float reach = (1.0f - age_factor) * (float)rows;
                if ((float)drow <= reach + 1.0f) {
                    float arm = 1.0f - (float)drow / (reach + 1.0f);
                    if (arm > spatial) spatial = arm;
                }
            }

            if (spatial <= 0)
                continue;

            float intensity = age_factor * spatial * brt;
            struct color_rgb_float c = {
                .r = rgb.r * intensity,
                .g = rgb.g * intensity,
                .b = rgb.b * intensity,
            };

            if (c.r > fx_pixels[j].r) fx_pixels[j].r = c.r;
            if (c.g > fx_pixels[j].g) fx_pixels[j].g = c.g;
            if (c.b > fx_pixels[j].b) fx_pixels[j].b = c.b;
        }

        rn_events[e].age++;
    }
}
