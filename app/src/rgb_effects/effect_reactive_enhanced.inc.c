/* REACTIVE ENHANCED: variant of reactive with larger fade and color interpolation */

/* Keypress handler: light up ALL LEDs mapped to the pressed key position.
 * Supports multi-LED sharing (multiple strip LEDs → same binding). */
static void reactive_add_event(uint32_t position) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        if ((uint32_t)effect_pixel_lookup(i) == position) {
            reactive_brightness[i] = 255;
        }
    }
}

static void zmk_rgb_underglow_effect_reactive_enhanced(void) {
    struct color_rgb_float base_rgb;
    user_color_rgb_float(&base_rgb);
    float brt = get_brightness_factor();

    /* Decay rate scales with animation speed for responsiveness */
    uint8_t decay = 2 + (int)anim_speed();

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t b = reactive_brightness[i];
        if (b == 0) {
            fx_pixels[i] = (struct color_rgb_float){0, 0, 0};
        } else {
            float factor = (float)b / 255.0f;
            /* make fade longer by squaring factor for smoother tail */
            factor = factor * factor;
            float f = brt * factor;

            fx_pixels[i] = (struct color_rgb_float){
                .r = base_rgb.r * f, .g = base_rgb.g * f, .b = base_rgb.b * f};

            /* decay */
            if (reactive_brightness[i] > decay)
                reactive_brightness[i] -= decay;
            else
                reactive_brightness[i] = 0;
        }
    }
}

static void reactive_enhanced_reset(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++)
        reactive_brightness[i] = 0;
}
