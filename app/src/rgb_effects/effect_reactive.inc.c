/* REACTIVE effect: keys light up on press and fade out over time.
 * Inspired by QMK's SOLID_REACTIVE_SIMPLE effect.
 *
 * Each keypress sets the corresponding pixel to full brightness.
 * All lit pixels fade out smoothly each frame. Fade speed is controlled
 * by animation_speed. Multiple keys can be active simultaneously.
 */

static void reactive_add_event(uint32_t position) {
    uint32_t pixel = position % STRIP_NUM_PIXELS;
    reactive_brightness[pixel] = 255;
}

static void reactive_reset(void) {
    memset(reactive_brightness, 0, sizeof(reactive_brightness));
}

static void zmk_rgb_underglow_effect_reactive(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float base_color;
    hsl_to_rgb_float(&hsl, &base_color);
    float brt = get_brightness_factor();

    /* Fade speed scales with animation_speed: higher speed = faster fade */
    uint8_t fade_step = 3 + state.animation_speed * 3;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        if (reactive_brightness[i] > 0) {
            float intensity = (float)reactive_brightness[i] / 255.0f * brt;
            fx_pixels[i].r = intensity * base_color.r;
            fx_pixels[i].g = intensity * base_color.g;
            fx_pixels[i].b = intensity * base_color.b;

            /* Fade out */
            if (reactive_brightness[i] > fade_step) {
                reactive_brightness[i] -= fade_step;
            } else {
                reactive_brightness[i] = 0;
            }
        } else {
            fx_pixels[i].r = 0;
            fx_pixels[i].g = 0;
            fx_pixels[i].b = 0;
        }
    }
}
