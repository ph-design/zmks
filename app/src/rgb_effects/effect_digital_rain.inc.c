/* DIGITAL RAIN effect: Matrix-style falling "raindrops" streaming down
 * in columns, with a bright head and a fading green tail.
 *
 * Each column independently spawns drops that fall from top to bottom.
 * After a drop clears the column, a random delay occurs before the next
 * one begins, creating a natural staggered look.
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
    int8_t head_row;     /* current row of drop head (-trail .. rows+trail) */
    uint8_t trail_len;   /* 2-5 rows of glowing trail behind the head */
    uint8_t delay;       /* frames to wait before spawning next drop */
    bool active;         /* true when a drop is falling */
};

static struct rain_column rain_cols_state[DRAIN_MAX_COLS];
static bool rain_initialized = false;

/* Grid helpers (local) */
static int dr_cols(void) {
    if (STRIP_NUM_PIXELS >= 120) return STRIP_NUM_PIXELS / 5;
    if (STRIP_NUM_PIXELS >= 48)  return STRIP_NUM_PIXELS / 4;
    if (STRIP_NUM_PIXELS >= 30)  return STRIP_NUM_PIXELS / 3;
    return STRIP_NUM_PIXELS;
}

static int dr_rows(void) {
    int c = dr_cols();
    return (c > 0) ? ((STRIP_NUM_PIXELS + c - 1) / c) : 1;
}

/* Spawn a new drop for column c */
static void rain_spawn(int c, int rows) {
    rain_cols_state[c].head_row = -1;                    /* start above top */
    rain_cols_state[c].trail_len = 2 + (rand() % 4);    /* 2-5 */
    rain_cols_state[c].delay = 0;
    rain_cols_state[c].active = true;
}

static void digital_rain_reset(void) {
    int cols = dr_cols();
    if (cols > DRAIN_MAX_COLS) cols = DRAIN_MAX_COLS;
    int rows = dr_rows();

    for (int c = 0; c < cols; c++) {
        /* Stagger initial drops: some active at random rows, some delayed */
        if (rand() % 3 == 0) {
            rain_spawn(c, rows);
            rain_cols_state[c].head_row = rand() % rows;
        } else {
            rain_cols_state[c].active = false;
            rain_cols_state[c].delay = rand() % 30;
        }
    }
    rain_initialized = true;
}

static void zmk_rgb_underglow_effect_digital_rain(void) {
    float brt = get_brightness_factor();

    int cols = dr_cols();
    if (cols > DRAIN_MAX_COLS) cols = DRAIN_MAX_COLS;
    int rows = dr_rows();
    if (rows < 1) rows = 1;

    /* Lazy init on first render (in case reset wasn't called) */
    if (!rain_initialized)
        digital_rain_reset();

    /* Fall rate: advance head every N frames.
     * Speed 1 → every 5 frames (12 rows/s)
     * Speed 3 → every 2 frames (30 rows/s)
     * Speed 5 → every frame  (60 rows/s)
     */
    static uint8_t rain_frame_counter = 0;
    rain_frame_counter++;
    uint8_t frames_per_step = 6 - state.animation_speed;
    if (frames_per_step < 1) frames_per_step = 1;
    bool advance = (rain_frame_counter >= frames_per_step);
    if (advance) rain_frame_counter = 0;

    /* Base hue for the rain — default green (120°), shifted by user H */
    float base_hue = hue_wrap(120.0f + (float)state.color.h);

    /* Clear canvas */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i] = (struct color_rgb_float){0, 0, 0};
    }

    for (int c = 0; c < cols; c++) {
        struct rain_column *rc = &rain_cols_state[c];

        if (!rc->active) {
            /* Countdown delay, then spawn */
            if (advance) {
                if (rc->delay > 0) {
                    rc->delay--;
                } else {
                    rain_spawn(c, rows);
                }
            }
            continue;
        }

        /* Render the drop: head row is brightest, trail rows fade */
        for (int t = 0; t <= (int)rc->trail_len; t++) {
            int r = rc->head_row - t;
            if (r < 0 || r >= rows)
                continue;

            int pixel_idx = r * cols + c;
            if (pixel_idx >= STRIP_NUM_PIXELS)
                continue;

            float trail_factor;
            if (t == 0) {
                /* Head: bright white-green */
                trail_factor = 1.0f;
            } else {
                /* Trail: exponential fade */
                trail_factor = 1.0f - (float)t / ((float)rc->trail_len + 1.0f);
                trail_factor *= trail_factor;
            }

            float intensity = trail_factor * brt;

            /* Head pixel is slightly brighter/whiter, trail is more saturated */
            struct color_hsl pixel_hsl;
            if (t == 0) {
                /* Head: lower saturation for a white-green flash */
                pixel_hsl = (struct color_hsl){
                    (uint16_t)base_hue,
                    (uint16_t)(state.color.s * 0.5f),
                    (uint16_t)(50 + 20)};  /* brighter lightness */
            } else {
                pixel_hsl = (struct color_hsl){
                    (uint16_t)base_hue,
                    state.color.s,
                    50};
            }

            struct color_rgb_float rgb;
            hsl_to_rgb_float(&pixel_hsl, &rgb);

            struct color_rgb_float color = {
                .r = rgb.r * intensity,
                .g = rgb.g * intensity,
                .b = rgb.b * intensity,
            };

            /* Lighten blend (multiple drops can overlap at column edges) */
            if (color.r > fx_pixels[pixel_idx].r)
                fx_pixels[pixel_idx].r = color.r;
            if (color.g > fx_pixels[pixel_idx].g)
                fx_pixels[pixel_idx].g = color.g;
            if (color.b > fx_pixels[pixel_idx].b)
                fx_pixels[pixel_idx].b = color.b;
        }

        /* Advance the drop */
        if (advance) {
            rc->head_row++;

            /* Drop finished when even the tail has left the bottom */
            if (rc->head_row - (int)rc->trail_len >= rows) {
                rc->active = false;
                /* Random delay before next drop: 5-35 frames */
                rc->delay = 5 + (rand() % 30);
            }
        }
    }
}
