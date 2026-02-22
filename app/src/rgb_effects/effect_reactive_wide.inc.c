/* REACTIVE WIDE effect: hue & value pulse radiating outward from the
 * pressed key's physical position, affecting a wide area of neighbouring
 * LEDs.  Multiple simultaneous presses are supported.
 *
 * Uses physical 2D coordinates (led_norm_x/y) for proper spatial distance
 * when led-positions are available.  Falls back to 1D strip distance when not.
 *
 * Inspired by QMK RGB_MATRIX_SOLID_REACTIVE_MULTIWIDE.
 */

#define REACTIVE_WIDE_MAX_EVENTS 16
#define REACTIVE_WIDE_RADIUS_F 0.35f /* normalised distance (0-1) radius */

struct reactive_wide_event {
    float src_x; /* source key X (normalised 0-1) */
    float src_y; /* source key Y (normalised 0-1) */
    uint8_t age;
    bool active;
};

static struct reactive_wide_event rw_events[REACTIVE_WIDE_MAX_EVENTS];

static void reactive_wide_add_event(uint32_t position) {
    float sx = key_src_x(position);
    float sy = key_src_y(position);

    /* Find a free slot, or overwrite the oldest */
    int oldest = 0;
    uint8_t oldest_age = 0;
    for (int i = 0; i < REACTIVE_WIDE_MAX_EVENTS; i++) {
        if (!rw_events[i].active) {
            rw_events[i].src_x = sx;
            rw_events[i].src_y = sy;
            rw_events[i].age = 0;
            rw_events[i].active = true;
            return;
        }
        if (rw_events[i].age > oldest_age) {
            oldest_age = rw_events[i].age;
            oldest = i;
        }
    }
    /* Reuse oldest */
    rw_events[oldest].src_x = sx;
    rw_events[oldest].src_y = sy;
    rw_events[oldest].age = 0;
    rw_events[oldest].active = true;
}

static void reactive_wide_reset(void) {
    for (int i = 0; i < REACTIVE_WIDE_MAX_EVENTS; i++)
        rw_events[i].active = false;
}

static void zmk_rgb_underglow_effect_reactive_wide(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();

    /* Max lifetime in frames — shorter at higher speed */
    uint8_t max_age = 120 / (state.animation_speed > 0 ? state.animation_speed : 1);
    if (max_age < 10)
        max_age = 10;

    /* Base is off */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i] = (struct color_rgb_float){0, 0, 0};
    }

    for (int e = 0; e < REACTIVE_WIDE_MAX_EVENTS; e++) {
        if (!rw_events[e].active)
            continue;

        float age_factor = 1.0f - (float)rw_events[e].age / (float)max_age;
        if (age_factor <= 0) {
            rw_events[e].active = false;
            continue;
        }
        /* Square for smoother fade-out tail */
        age_factor *= age_factor;

        /* Hue shifts toward complementary as age increases */
        struct color_hsl shifted = hsl;
        shifted.h = (hsl.h + (uint16_t)((1.0f - age_factor) * 60)) % 360;
        struct color_rgb_float rgb;
        hsl_to_rgb_float(&shifted, &rgb);

        for (int j = 0; j < STRIP_NUM_PIXELS; j++) {
            /* 2D Euclidean distance using physical coordinates */
            float dx = led_norm_x(j) - rw_events[e].src_x;
            float dy = led_norm_y(j) - rw_events[e].src_y;
            float dist = sqrtf(dx * dx + dy * dy);

            if (dist > REACTIVE_WIDE_RADIUS_F)
                continue;

            float spatial = 1.0f - dist / REACTIVE_WIDE_RADIUS_F;
            float intensity = age_factor * spatial * brt;

            struct color_rgb_float c = {
                .r = rgb.r * intensity,
                .g = rgb.g * intensity,
                .b = rgb.b * intensity,
            };

            /* Lighten blend */
            if (c.r > fx_pixels[j].r)
                fx_pixels[j].r = c.r;
            if (c.g > fx_pixels[j].g)
                fx_pixels[j].g = c.g;
            if (c.b > fx_pixels[j].b)
                fx_pixels[j].b = c.b;
        }

        rw_events[e].age++;
    }
}
