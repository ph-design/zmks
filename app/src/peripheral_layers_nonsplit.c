/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Stub implementation of peripheral_layers for non-split keyboards.
 * The split_peripheral_layer_changed event is never raised on non-split boards,
 * so these functions are never called at runtime, but the linker needs the symbols.
 */

#include <stdint.h>
#include <stdbool.h>

void set_peripheral_layers_state(uint32_t new_layers) {}

bool peripheral_layer_active(uint8_t layer) { return false; }

uint8_t peripheral_highest_layer_active(void) { return 0; }
