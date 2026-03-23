/* SPARKLE effect (moved out of rgb_underglow.c) */
static void zmk_rgb_underglow_effect_sparkle(void) {
    float brt = get_brightness_factor();

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        if (sparkle_data[i].counter == 0) {
            sparkle_generate_pixel(i, false);
        }

        sparkle_data[i].counter--;

        float intensity;
        if (sparkle_data[i].total_frames <= sparkle_data[i].counter) {
            /* Fade in phase */
            intensity = 2.0f - (sparkle_data[i].step * (float)(sparkle_data[i].counter));
        } else {
            /* Fade out phase */
            intensity = sparkle_data[i].step * (float)(sparkle_data[i].counter);
        }
        if (intensity < 0.0f)
            intensity = 0.0f;
        if (intensity > 1.0f)
            intensity = 1.0f;

        fx_pixels[i].r = intensity * sparkle_data[i].color.r * brt;
        fx_pixels[i].g = intensity * sparkle_data[i].color.g * brt;
        fx_pixels[i].b = intensity * sparkle_data[i].color.b * brt;
    }
}

static void sparkle_init_all(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        sparkle_generate_pixel(i, true);
    }
}
