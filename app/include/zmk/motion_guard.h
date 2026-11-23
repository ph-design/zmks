/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct mg_config {
    uint32_t carried_settle_ms;
    uint32_t brief_settle_ms;
    uint32_t stable_span_mg;
    uint32_t departure_mg;
    uint32_t verify_ms;
};

void zmk_motion_guard_key(uint32_t position, bool pressed, int64_t timestamp);
void zmk_motion_guard_set_enabled(bool enabled);
void zmk_motion_guard_get_config(struct mg_config *out);
bool zmk_motion_guard_config_valid(const struct mg_config *config);
int zmk_motion_guard_set_config(const struct mg_config *config);
int zmk_motion_guard_save(void);
int zmk_motion_guard_reset(void);
bool zmk_motion_guard_locked(void);
uint32_t zmk_motion_guard_generation(void);
