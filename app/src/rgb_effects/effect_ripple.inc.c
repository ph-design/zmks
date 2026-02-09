/* RIPPLE effect (moved out of rgb_underglow.c) */
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

    /* Render each active ripple event
     * Protect ring buffer access with a mutex to avoid races between the
     * event listener (calling ripple_add_event) and the rendering tick.
     */
    k_mutex_lock(&ripple_mutex, K_FOREVER);
    uint8_t idx = ripple_events_start;
    int processed = 0;
    while (idx != ripple_events_end && processed < ripple_num_events) {
        struct ripple_event *event = &ripple_events[idx];
        /* Copy event locally to minimize time holding the mutex while rendering */
        struct ripple_event local_event = *event;
        k_mutex_unlock(&ripple_mutex);

        for (int j = 0; j < STRIP_NUM_PIXELS; j++) {
            /* 1D distance: scale pixel index difference to 0-255 range */
            int pixel_dist =
                (int)(((float)abs((int)j - (int)local_event.pixel_id) / (float)STRIP_NUM_PIXELS) *
                      255);

            int diff = abs(pixel_dist - (int)local_event.distance);
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

        /* Advance the ripple -- update shared buffer under mutex */
        k_mutex_lock(&ripple_mutex, K_FOREVER);
        /* Re-fetch pointer in case buffer rotated */
        struct ripple_event *pe = &ripple_events[idx];
        if (pe->counter < event_frames) {
            pe->distance += distance_per_frame;
            pe->counter++;
        } else {
            ripple_events_start = (ripple_events_start + 1) % RIPPLE_MAX_EVENTS;
            ripple_num_events--;
        }

        idx = (idx + 1) % RIPPLE_MAX_EVENTS;
        processed++;
    }
    k_mutex_unlock(&ripple_mutex);
}
