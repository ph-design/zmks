/* KNIGHT effect (back-and-forth single segment) — uses physical X position */
static float knight_pos_f = 0.0f;
static int knight_dir_f = 1;
static uint8_t knight_frame_acc = 0;
static void zmk_rgb_underglow_effect_knight(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float rgb;
    hsl_to_rgb_float(&hsl, &rgb);
    float brt = get_brightness_factor();

    float len = 1.0f / 6.0f; /* segment width in normalised space */
    /* Pre-scale colour once */
    struct color_rgb_float base = {rgb.r * brt, rgb.g * brt, rgb.b * brt};

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        float px = led_norm_x(i);
        float rel = fabsf(px - knight_pos_f);
        if (rel < len) {
            float factor = 1.0f - rel / len;
            /* Quadratic falloff for smoother tails */
            factor *= factor;
            fx_pixels[i] = (struct color_rgb_float){
                .r = base.r * factor, .g = base.g * factor, .b = base.b * factor};
        } else {
            fx_pixels[i] = (struct color_rgb_float){0, 0, 0};
        }
    }

    /* Frame accumulator for sub-pixel smooth motion at low speeds */
    knight_frame_acc += state.animation_speed;
    if (knight_frame_acc >= 2) {
        int steps = knight_frame_acc / 2;
        knight_frame_acc %= 2;
        float delta = (float)knight_dir_f * (float)steps * 0.015f;
        knight_pos_f += delta;
        if (knight_pos_f >= 1.0f) {
            knight_pos_f = 1.0f;
            knight_dir_f = -1;
        } else if (knight_pos_f <= 0.0f) {
            knight_pos_f = 0.0f;
            knight_dir_f = 1;
        }
    }
}

static void knight_reset(void) {
    knight_pos_f = 0.0f;
    knight_dir_f = 1;
    knight_frame_acc = 0;
}
