/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

bool zmk_2g4_crypto_enabled(void);
uint32_t zmk_2g4_crypto_tx_session_id(void);
int zmk_2g4_crypto_encrypt(uint8_t *data, size_t len, size_t buf_size);
int zmk_2g4_crypto_decrypt(uint8_t *data, size_t len);
