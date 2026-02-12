/* KNIGHT effect (back-and-forth single segment) */
static int knight_pos = 0;
static int knight_dir = 1;
static void zmk_rgb_underglow_effect_knight(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float rgb;
    hsl_to_rgb_float(&hsl, &rgb);
    float brt = get_brightness_factor();

    int len = MAX(1, STRIP_NUM_PIXELS / 6);
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int rel = abs(i - knight_pos);
        float factor = (rel < len) ? (1.0f - (float)rel / (float)len) : 0.0f;
        struct color_rgb_float out = rgb;
        out.r *= brt * factor;
        out.g *= brt * factor;
        out.b *= brt * factor;
        fx_pixels[i] = out;
    }

    /* Advance position */
    knight_pos += knight_dir * (int)state.animation_speed;
    if (knight_pos >= STRIP_NUM_PIXELS) {
        knight_pos = STRIP_NUM_PIXELS - 1;
        knight_dir = -1;
    } else if (knight_pos < 0) {
        knight_pos = 0;
        knight_dir = 1;
    }
}

static void knight_reset(void) {
    knight_pos = 0;
    knight_dir = 1;
}
