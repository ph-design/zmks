/* SPIRAL OUT effect (center moves outward) — uses 2D physical coords */
static float spiral_out_phase = 0.0f;
static void zmk_rgb_underglow_effect_spiral_out(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
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
        struct color_rgb_float rgb;
        hsl_to_rgb_float(&hsl, &rgb);
        rgb.r *= brt * v;
        rgb.g *= brt * v;
        rgb.b *= brt * v;
        fx_pixels[i] = rgb;
    }

    spiral_out_phase += anim_speed() * 0.006f;
    if (spiral_out_phase >= 1.0f)
        spiral_out_phase -= 1.0f;
}

static void spiral_out_reset(void) { spiral_out_phase = 0.0f; }
