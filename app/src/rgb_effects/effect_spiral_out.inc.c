/* SPIRAL OUT effect (center moves outward) */
static float spiral_out_center = 0.0f;
static void zmk_rgb_underglow_effect_spiral_out(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    float brt = get_brightness_factor();

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        float dist = fabsf((float)i - spiral_out_center);
        if (dist > STRIP_NUM_PIXELS / 2)
            dist = STRIP_NUM_PIXELS - dist;
        float v = dist / (STRIP_NUM_PIXELS / 2.0f);
        v = CLAMP(v, 0.0f, 1.0f);
        struct color_rgb_float rgb;
        hsl_to_rgb_float(&hsl, &rgb);
        rgb.r *= brt * v;
        rgb.g *= brt * v;
        rgb.b *= brt * v;
        fx_pixels[i] = rgb;
    }

    spiral_out_center += (float)state.animation_speed * 0.4f;
    if (spiral_out_center >= STRIP_NUM_PIXELS)
        spiral_out_center -= STRIP_NUM_PIXELS;
}

static void spiral_out_reset(void) {
    spiral_out_center = 0.0f;
}
