/* SPLASH effect: Rainbow-coloured expanding rings from keypress positions.
 *
 * Similar to RIPPLE but each ring shows a different hue based on the
 * distance from the splash origin, creating a full rainbow pattern that
 * expands outward and fades with age.  Multiple simultaneous splashes
 * are supported with lighten blending.
 *
 * Controls:
 *   H  – base hue of the rainbow palette
 *   S  – saturation
 *   B  – brightness
 *   Speed – ring expansion rate & lifetime (1 = slow, 5 = fast)
 *
 * QMK equivalent: SPLASH / MULTISPLASH
 */

#define SPLASH_MAX_EVENTS  16
#define SPLASH_RING_WIDTH  45   /* width of each ring in 0-255 distance space */

struct splash_event {
    uint32_t pixel_id;  /* origin position (matrix index) */
    uint8_t  age;       /* frames since event fired */
    bool     active;
};

static struct splash_event splash_events[SPLASH_MAX_EVENTS];

static void splash_add_event(uint32_t position) {
    if (position >= STRIP_NUM_PIXELS)
        return;

    /* Find free slot or recycle oldest */
    int oldest = 0;
    uint8_t oldest_age = 0;
    for (int i = 0; i < SPLASH_MAX_EVENTS; i++) {
        if (!splash_events[i].active) {
            splash_events[i].pixel_id = position;
            splash_events[i].age = 0;
            splash_events[i].active = true;
            return;
        }
        if (splash_events[i].age > oldest_age) {
            oldest_age = splash_events[i].age;
            oldest = i;
        }
    }
    splash_events[oldest].pixel_id = position;
    splash_events[oldest].age = 0;
    splash_events[oldest].active = true;
}

static void splash_reset(void) {
    for (int i = 0; i < SPLASH_MAX_EVENTS; i++)
        splash_events[i].active = false;
}

static void zmk_rgb_underglow_effect_splash(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();

    /* Max lifetime in frames.
     * Speed 1 → 100 frames (1.7 s)   Speed 3 → 33 frames (0.55 s)
     * Speed 5 → 20 frames (0.33 s)
     */
    uint8_t max_age = 100 / (state.animation_speed > 0 ? state.animation_speed : 1);
    if (max_age < 10)
        max_age = 10;

    /* Clear canvas */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i] = (struct color_rgb_float){0, 0, 0};
    }

    /* Ring expansion speed in 0-255 distance space per frame.
     * At max_age frames the ring should roughly cover the whole keyboard
     * (distance 255).  So: expansion ≈ 255 / max_age.
     */
    float expansion_per_frame = 255.0f / (float)max_age;

    for (int e = 0; e < SPLASH_MAX_EVENTS; e++) {
        if (!splash_events[e].active)
            continue;

        float age_norm = (float)splash_events[e].age / (float)max_age;
        if (age_norm >= 1.0f) {
            splash_events[e].active = false;
            continue;
        }

        /* Fade intensity as the splash ages */
        float age_fade = 1.0f - age_norm;
        age_fade *= age_fade; /* Quadratic for smoother tail */

        /* Current ring radius in distance space */
        float ring_center = (float)splash_events[e].age * expansion_per_frame;

        for (int j = 0; j < STRIP_NUM_PIXELS; j++) {
            /* Distance from splash origin (normalised to 0-255) */
            float raw_dist = fabsf((float)j - (float)splash_events[e].pixel_id);
            float pixel_dist = (raw_dist / (float)STRIP_NUM_PIXELS) * 255.0f;

            /* How close is this pixel to the expanding ring edge? */
            float diff = fabsf(pixel_dist - ring_center);
            if (diff >= (float)SPLASH_RING_WIDTH)
                continue;

            float ring_factor = 1.0f - diff / (float)SPLASH_RING_WIDTH;

            /* Rainbow hue based on normalised distance from origin.
             * Closer pixels = base hue, further = shifted hue.
             * Spread the full 360° across the keyboard width.
             */
            float hue = hue_wrap(
                (float)state.color.h + pixel_dist * (360.0f / 255.0f));

            struct color_hsl pixel_hsl = {(uint16_t)hue, hsl.s, hsl.l};
            struct color_rgb_float rgb;
            hsl_to_rgb_float(&pixel_hsl, &rgb);

            float intensity = ring_factor * age_fade * brt;
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

        splash_events[e].age++;
    }
}
