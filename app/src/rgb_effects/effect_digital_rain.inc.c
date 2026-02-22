/* DIGITAL RAIN effect: Matrix-style falling "raindrops" streaming down
 * in columns, with a bright head and a fading green tail.
 *
 * Uses physical coordinates when available: X determines the column,
 * Y determines the row position.  Falls back to grid approximation.
 *
 * Controls:
 *   H  – tints the green palette (0 = pure green, adjustable)
 *   S  – saturation (100 = vivid green, lower = whiter)
 *   B  – brightness
 *   Speed – fall rate (1 = slow drip, 5 = downpour)
 *
 * QMK equivalent: DIGITAL_RAIN
 */

/* Maximum columns we'll support (enough for the largest keyboards) */
#define DRAIN_MAX_COLS 32

struct rain_column {
    float head_y;    /* current Y position of drop head (0-1 normalised) */
    float trail_len; /* trail length in normalised Y space */
    uint8_t delay;   /* frames to wait before spawning next drop */
    bool active;     /* true when a drop is falling */
};

static struct rain_column rain_cols_state[DRAIN_MAX_COLS];
static float rain_col_x[DRAIN_MAX_COLS]; /* X position of each column */
static int rain_num_cols = 0;
static bool rain_initialized = false;

/* Build column structure from LED positions */
static void rain_build_columns(void) {
    /* Quantise LED X positions into columns */
    rain_num_cols = 0;
    for (int i = 0; i < STRIP_NUM_PIXELS && rain_num_cols < DRAIN_MAX_COLS; i++) {
        float x = led_norm_x(i);
        bool found = false;
        for (int c = 0; c < rain_num_cols; c++) {
            if (fabsf(rain_col_x[c] - x) < 0.03f) {
                found = true;
                break;
            }
        }
        if (!found) {
            rain_col_x[rain_num_cols] = x;
            rain_num_cols++;
        }
    }
    /* Sort columns left to right (simple bubble sort, small N) */
    for (int a = 0; a < rain_num_cols - 1; a++) {
        for (int b = a + 1; b < rain_num_cols; b++) {
            if (rain_col_x[b] < rain_col_x[a]) {
                float tmp = rain_col_x[a];
                rain_col_x[a] = rain_col_x[b];
                rain_col_x[b] = tmp;
            }
        }
    }
}

/* Spawn a new drop for column c */
static void rain_spawn(int c) {
    rain_cols_state[c].head_y = -0.1f; /* start above top */
    rain_cols_state[c].trail_len = 0.15f + (float)(rand() % 20) * 0.01f;
    rain_cols_state[c].delay = 0;
    rain_cols_state[c].active = true;
}

static void digital_rain_reset(void) {
    rain_build_columns();
    int cols = rain_num_cols;

    for (int c = 0; c < cols; c++) {
        /* Stagger initial drops */
        if (rand() % 3 == 0) {
            rain_spawn(c);
            rain_cols_state[c].head_y = (float)(rand() % 100) / 100.0f;
        } else {
            rain_cols_state[c].active = false;
            rain_cols_state[c].delay = rand() % 30;
        }
    }
    rain_initialized = true;
}

static void zmk_rgb_underglow_effect_digital_rain(void) {
    float brt = get_brightness_factor();

    /* Lazy init on first render */
    if (!rain_initialized)
        digital_rain_reset();

    int cols = rain_num_cols;
    if (cols < 1)
        return;

    /* Fall rate: advance head every N frames */
    static uint8_t rain_frame_counter = 0;
    rain_frame_counter++;
    uint8_t frames_per_step = 6 - state.animation_speed;
    if (frames_per_step < 1)
        frames_per_step = 1;
    bool advance = (rain_frame_counter >= frames_per_step);
    if (advance)
        rain_frame_counter = 0;

    /* Base hue for the rain */
    float base_hue = hue_wrap(120.0f + (float)state.color.h);

    /* Clear canvas */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i] = (struct color_rgb_float){0, 0, 0};
    }

    float fall_speed = 0.02f + (float)state.animation_speed * 0.01f;

    for (int c = 0; c < cols; c++) {
        struct rain_column *rc = &rain_cols_state[c];

        if (!rc->active) {
            if (advance) {
                if (rc->delay > 0) {
                    rc->delay--;
                } else {
                    rain_spawn(c);
                }
            }
            continue;
        }

        /* Render: for each LED, check if it's in this column and within the trail */
        for (int j = 0; j < STRIP_NUM_PIXELS; j++) {
            float jx = led_norm_x(j);
            /* Check if LED is in this column (within tolerance) */
            if (fabsf(jx - rain_col_x[c]) > 0.03f)
                continue;

            float jy = led_norm_y(j);
            float dist_from_head = rc->head_y - jy;
            if (dist_from_head < 0 || dist_from_head > rc->trail_len)
                continue;

            float trail_factor;
            if (dist_from_head < 0.02f) {
                /* Head: bright */
                trail_factor = 1.0f;
            } else {
                trail_factor = 1.0f - dist_from_head / rc->trail_len;
                trail_factor *= trail_factor;
            }

            float intensity = trail_factor * brt;

            struct color_hsl pixel_hsl;
            if (dist_from_head < 0.02f) {
                pixel_hsl =
                    (struct color_hsl){(uint16_t)base_hue, (uint16_t)(state.color.s * 0.5f), 70};
            } else {
                pixel_hsl = (struct color_hsl){(uint16_t)base_hue, state.color.s, 50};
            }

            struct color_rgb_float rgb;
            hsl_to_rgb_float(&pixel_hsl, &rgb);

            struct color_rgb_float color = {
                .r = rgb.r * intensity,
                .g = rgb.g * intensity,
                .b = rgb.b * intensity,
            };

            if (color.r > fx_pixels[j].r)
                fx_pixels[j].r = color.r;
            if (color.g > fx_pixels[j].g)
                fx_pixels[j].g = color.g;
            if (color.b > fx_pixels[j].b)
                fx_pixels[j].b = color.b;
        }

        /* Advance the drop */
        if (advance) {
            rc->head_y += fall_speed;
            if (rc->head_y - rc->trail_len > 1.0f) {
                rc->active = false;
                rc->delay = 5 + (rand() % 30);
            }
        }
    }
}
