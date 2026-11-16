/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#if defined(CONFIG_ZMK_STUDIO_TRANSPORT_2G4)
void studio_radio_received(const uint8_t *data, size_t len);
void studio_radio_started(void);
void studio_radio_status(char *out, size_t size);
#else
static inline void studio_radio_received(const uint8_t *data, size_t len) {
    (void)data;
    (void)len;
}
static inline void studio_radio_started(void) {}
static inline void studio_radio_status(char *out, size_t size) {
    snprintf(out, size, "STUDIO_2G4=0\r\n");
}
#endif
