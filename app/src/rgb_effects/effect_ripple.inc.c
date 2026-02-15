/* RIPPLE effect: expanding rings from key press positions */

static void ripple_add_event(uint32_t position) {
    if (position >= STRIP_NUM_PIXELS)
        return;
    k_mutex_lock(&ripple_mutex, K_FOREVER);
    if (ripple_num_events >= RIPPLE_MAX_EVENTS) {
        /* Drop oldest event */
        ripple_events_start = (ripple_events_start + 1) % RIPPLE_MAX_EVENTS;
        ripple_num_events--;
    }

    uint8_t end = (ripple_events_start + ripple_num_events) % RIPPLE_MAX_EVENTS;
    ripple_events[end].pixel_id = position; /* key matrix position */
    ripple_events[end].distance = 0;
    ripple_events[end].counter = 0;
    ripple_num_events++;
    k_mutex_unlock(&ripple_mutex);
}

static void ripple_reset(void) {
    k_mutex_lock(&ripple_mutex, K_FOREVER);
    ripple_events_start = 0;
    ripple_num_events = 0;
    k_mutex_unlock(&ripple_mutex);
}

static void zmk_rgb_underglow_effect_ripple(void) {
    struct color_hsl hsl = hsb_to_hsl(state.color);
    struct color_rgb_float base_color;
    hsl_to_rgb_float(&hsl, &base_color);
    float brt = get_brightness_factor();

    /* Clear to black */
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        fx_pixels[i].r = 0;
        fx_pixels[i].g = 0;
        fx_pixels[i].b = 0;
    }

    /* Ripple speed scales with animation_speed */
    uint8_t distance_per_frame = 3 + state.animation_speed * 2;
    uint8_t event_frames = 255 / distance_per_frame;
    if (event_frames < 1)
        event_frames = 1;

    /* Snapshot active events under mutex, then render without holding it. */
    k_mutex_lock(&ripple_mutex, K_FOREVER);
    uint8_t count = ripple_num_events;
    uint8_t start = ripple_events_start;
    struct ripple_event local_events[RIPPLE_MAX_EVENTS];
    for (int i = 0; i < count; i++) {
        local_events[i] = ripple_events[(start + i) % RIPPLE_MAX_EVENTS];
    }
    k_mutex_unlock(&ripple_mutex);

    /* Render each active ripple event (no lock held) */
    for (int i = 0; i < count; i++) {
        struct ripple_event *ev = &local_events[i];

        for (int j = 0; j < STRIP_NUM_PIXELS; j++) {
            int pixel_dist =
                (int)(((float)abs(j - (int)ev->pixel_id) / (float)STRIP_NUM_PIXELS) * 255);

            int diff = abs(pixel_dist - (int)ev->distance);
            if (diff < RIPPLE_WIDTH) {
                float intensity = (1.0f - (float)diff / (float)RIPPLE_WIDTH) * brt;

                struct color_rgb_float color = {
                    .r = intensity * base_color.r,
                    .g = intensity * base_color.g,
                    .b = intensity * base_color.b,
                };

                /* Lighten blending: take max of each channel */
                if (color.r > fx_pixels[j].r)
                    fx_pixels[j].r = color.r;
                if (color.g > fx_pixels[j].g)
                    fx_pixels[j].g = color.g;
                if (color.b > fx_pixels[j].b)
                    fx_pixels[j].b = color.b;
            }
        }
    }

    /* Advance / expire ripple events under mutex */
    k_mutex_lock(&ripple_mutex, K_FOREVER);

    /* Phase 1: advance all snapshot events that are still alive */
    for (int i = 0; i < count; i++) {
        uint8_t slot = (start + i) % RIPPLE_MAX_EVENTS;
        struct ripple_event *pe = &ripple_events[slot];
        if (pe->counter < event_frames) {
            pe->distance += distance_per_frame;
            pe->counter++;
        }
        /* expired events left as-is; they will be removed below */
    }

    while (ripple_num_events > 0) {
        struct ripple_event *front = &ripple_events[ripple_events_start];
        if (front->counter >= event_frames) {
            ripple_events_start = (ripple_events_start + 1) % RIPPLE_MAX_EVENTS;
            ripple_num_events--;
        } else {
            break;
        }
    }

    k_mutex_unlock(&ripple_mutex);
}
