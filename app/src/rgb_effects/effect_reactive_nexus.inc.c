/* REACTIVE NEXUS effect: hue & value pulse outward along the same column
 * and row of the pressed key, forming a cross/nexus pattern.
 *
 * Uses physical 2D coordinates for proper cross alignment when led-positions
 * are available.  Falls back to grid approximation when not.
 *
 * Inspired by QMK RGB_MATRIX_SOLID_REACTIVE_MULTINEXUS.
 */

#define REACTIVE_NEXUS_MAX_EVENTS 16
#define NEXUS_ARM_TOLERANCE 0.08f /* cross-arm width in normalised space */

struct reactive_nexus_event {
    float src_x; /* source key X (normalised 0-1) */
    float src_y; /* source key Y (normalised 0-1) */
    uint8_t age;
    bool active;
};

static struct reactive_nexus_event rn_events[REACTIVE_NEXUS_MAX_EVENTS];

static void reactive_nexus_add_event(uint32_t position) {
    float sx = key_src_x(position);
    float sy = key_src_y(position);

    int oldest = 0;
    uint8_t oldest_age = 0;
    for (int i = 0; i < REACTIVE_NEXUS_MAX_EVENTS; i++) {
        if (!rn_events[i].active) {
            rn_events[i].src_x = sx;
            rn_events[i].src_y = sy;
            rn_events[i].age = 0;
            rn_events[i].active = true;
            return;
        }
        if (rn_events[i].age > oldest_age) {
            oldest_age = rn_events[i].age;
            oldest = i;
        }
    }
    rn_events[oldest].src_x = sx;
    rn_events[oldest].src_y = sy;
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

    uint8_t max_age = (uint8_t)(100.0f / anim_speed());
    if (max_age < 8)
        max_age = 8;

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

        /* Cross-arm reach expands as the event ages */
        float reach = (1.0f - age_factor) * 1.0f; /* max reach = full keyboard */

        for (int j = 0; j < STRIP_NUM_PIXELS; j++) {
            float jx = led_norm_x(j);
            float jy = led_norm_y(j);
            float dx = fabsf(jx - rn_events[e].src_x);
            float dy = fabsf(jy - rn_events[e].src_y);

            float spatial = 0.0f;

            /* Horizontal arm: LED is on same row (close Y) */
            if (dy < NEXUS_ARM_TOLERANCE && dx <= reach + 0.01f) {
                float arm = 1.0f - dx / (reach + 0.01f);
                float y_factor = 1.0f - dy / NEXUS_ARM_TOLERANCE;
                arm *= y_factor;
                if (arm > spatial)
                    spatial = arm;
            }
            /* Vertical arm: LED is on same column (close X) */
            if (dx < NEXUS_ARM_TOLERANCE && dy <= reach + 0.01f) {
                float arm = 1.0f - dy / (reach + 0.01f);
                float x_factor = 1.0f - dx / NEXUS_ARM_TOLERANCE;
                arm *= x_factor;
                if (arm > spatial)
                    spatial = arm;
            }

            if (spatial <= 0)
                continue;

            float intensity = age_factor * spatial * brt;
            struct color_rgb_float c = {
                .r = rgb.r * intensity,
                .g = rgb.g * intensity,
                .b = rgb.b * intensity,
            };

            if (c.r > fx_pixels[j].r)
                fx_pixels[j].r = c.r;
            if (c.g > fx_pixels[j].g)
                fx_pixels[j].g = c.g;
            if (c.b > fx_pixels[j].b)
                fx_pixels[j].b = c.b;
        }

        rn_events[e].age++;
    }
}
