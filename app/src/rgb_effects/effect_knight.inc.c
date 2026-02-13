/* KNIGHT effect (back-and-forth single segment) */
static int knight_pos = 0;
static int knight_dir = 1;
static uint8_t knight_frame_acc = 0;
static void zmk_rgb_underglow_effect_knight(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float rgb;
    hsl_to_rgb_float(&hsl, &rgb);
    float brt = get_brightness_factor();

    int len = MAX(1, STRIP_NUM_PIXELS / 6);
    /* Pre-scale colour once */
    struct color_rgb_float base = {rgb.r * brt, rgb.g * brt, rgb.b * brt};

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        int rel = i - knight_pos;
        if (rel < 0) rel = -rel;
        if (rel < len) {
            float factor = 1.0f - (float)rel / (float)len;
            /* Quadratic falloff for smoother tails */
            factor *= factor;
            fx_pixels[i] = (struct color_rgb_float){
                .r = base.r * factor,
                .g = base.g * factor,
                .b = base.b * factor
            };
        } else {
            fx_pixels[i] = (struct color_rgb_float){0, 0, 0};
        }
    }

    /* Frame accumulator for sub-pixel smooth motion at low speeds */
    knight_frame_acc += state.animation_speed;
    if (knight_frame_acc >= 2) {
        int steps = knight_frame_acc / 2;
        knight_frame_acc %= 2;
        knight_pos += knight_dir * steps;
        if (knight_pos >= STRIP_NUM_PIXELS) {
            knight_pos = STRIP_NUM_PIXELS - 1;
            knight_dir = -1;
        } else if (knight_pos < 0) {
            knight_pos = 0;
            knight_dir = 1;
        }
    }
}

static void knight_reset(void) {
    knight_pos = 0;
    knight_dir = 1;
    knight_frame_acc = 0;
}
