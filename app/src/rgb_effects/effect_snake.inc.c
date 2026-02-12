/* SNAKE effect (moving lit segment) */
static int snake_pos = 0;
static void zmk_rgb_underglow_effect_snake(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float rgb;
    hsl_to_rgb_float(&hsl, &rgb);
    float brt = get_brightness_factor();

    int len = MAX(1, STRIP_NUM_PIXELS / 8);
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int rel = (i - snake_pos + STRIP_NUM_PIXELS) % STRIP_NUM_PIXELS;
        float factor = (rel < len) ? (1.0f - (float)rel / (float)len) : 0.0f;
        struct color_rgb_float out = rgb;
        out.r *= brt * factor;
        out.g *= brt * factor;
        out.b *= brt * factor;
        fx_pixels[i] = out;
    }

    /* Advance position */
    snake_pos = (snake_pos + state.animation_speed) % STRIP_NUM_PIXELS;
}

static void snake_reset(void) {
    snake_pos = 0;
}
