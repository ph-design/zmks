/* TWINKLE / GLITTER effect — per-pixel lifecycle for smoother animation */
static uint8_t twinkle_brightness[STRIP_NUM_PIXELS];
static int8_t twinkle_dir[STRIP_NUM_PIXELS]; /* +1 brightening, -1 dimming, 0 idle */

static void zmk_rgb_underglow_effect_twinkle(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float base_rgb;
    hsl_to_rgb_float(&hsl, &base_rgb);
    float brt = get_brightness_factor();

    /* Probability of a new twinkle starting scales with speed */
    int threshold = 200 - state.animation_speed * 30;
    if (threshold < 20) threshold = 20;

    /* Step size for fade-in/out */
    uint8_t step = 4 + state.animation_speed * 2;
    if (step > 25) step = 25;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        /* Trigger new twinkle on idle pixels */
        if (twinkle_dir[i] == 0 && twinkle_brightness[i] == 0) {
            if ((rand() % threshold) == 0) {
                twinkle_dir[i] = 1;
                twinkle_brightness[i] = 1;
            }
        }

        /* Advance lifecycle */
        if (twinkle_dir[i] > 0) {
            int v = (int)twinkle_brightness[i] + step;
            if (v >= 255) {
                twinkle_brightness[i] = 255;
                twinkle_dir[i] = -1; /* start dimming */
            } else {
                twinkle_brightness[i] = (uint8_t)v;
            }
        } else if (twinkle_dir[i] < 0) {
            int v = (int)twinkle_brightness[i] - step;
            if (v <= 0) {
                twinkle_brightness[i] = 0;
                twinkle_dir[i] = 0; /* idle */
            } else {
                twinkle_brightness[i] = (uint8_t)v;
            }
        }

        /* Render */
        if (twinkle_brightness[i] > 0) {
            float f = brt * ((float)twinkle_brightness[i] / 255.0f);
            fx_pixels[i] = (struct color_rgb_float){
                .r = base_rgb.r * f,
                .g = base_rgb.g * f,
                .b = base_rgb.b * f
            };
        } else {
            fx_pixels[i] = (struct color_rgb_float){0, 0, 0};
        }
    }
}

static void twinkle_reset(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        twinkle_brightness[i] = 0;
        twinkle_dir[i] = 0;
        fx_pixels[i] = (struct color_rgb_float){0, 0, 0};
    }
}
