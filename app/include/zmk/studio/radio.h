/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>
#if defined(CONFIG_ZMK_STUDIO_TRANSPORT_2G4)
void studio_radio_received(const uint8_t *data, size_t len);
void studio_radio_started(void);
void studio_radio_tx_available(void);
#else
static inline void studio_radio_received(const uint8_t *data, size_t len) {
    (void)data;
    (void)len;
}
static inline void studio_radio_started(void) {}
static inline void studio_radio_tx_available(void) {}
#endif
