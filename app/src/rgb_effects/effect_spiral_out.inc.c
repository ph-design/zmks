/* SPIRAL OUT effect (center moves outward) — uses 2D physical coords */
static float spiral_out_phase = 0.0f;
static void zmk_rgb_underglow_effect_spiral_out(void) {
    struct color_rgb_float rgb;
    user_color_rgb_float(&rgb);
    float brt = get_brightness_factor();

    float cx = spiral_out_phase; /* move outward from left */
    float cy = 0.5f;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        float dx = led_norm_x(i) - cx;
        float dy = led_norm_y(i) - cy;
        float dist = sqrtf(dx * dx + dy * dy);
        /* Inverse: farther from center = brighter */
        float v = dist * 2.0f;
        v = CLAMP(v, 0.0f, 1.0f);
        fx_pixels[i] = (struct color_rgb_float){
            .r = rgb.r * brt * v,
            .g = rgb.g * brt * v,
            .b = rgb.b * brt * v,
        };
    }

    spiral_out_phase += anim_speed() * 0.006f;
    if (spiral_out_phase >= 1.0f)
        spiral_out_phase -= 1.0f;
}

static void spiral_out_reset(void) { spiral_out_phase = 0.0f; }
