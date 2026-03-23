/* SNAKE effect (moving lit segment) — uses physical X position */
static float snake_pos_f = 0.0f;
static void zmk_rgb_underglow_effect_snake(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float rgb;
    hsl_to_rgb_float(&hsl, &rgb);
    float brt = get_brightness_factor();

    float len = 1.0f / 8.0f; /* segment width normalised */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        float px = led_norm_x(i);
        float rel = px - snake_pos_f;
        if (rel < 0)
            rel += 1.0f;
        if (rel > 1.0f)
            rel -= 1.0f;
        float factor = (rel < len) ? (1.0f - rel / len) : 0.0f;
        struct color_rgb_float out = rgb;
        out.r *= brt * factor;
        out.g *= brt * factor;
        out.b *= brt * factor;
        fx_pixels[i] = out;
    }

    /* Advance position */
    snake_pos_f += anim_speed() * 0.005f;
    if (snake_pos_f >= 1.0f)
        snake_pos_f -= 1.0f;
}

static void snake_reset(void) { snake_pos_f = 0.0f; }
