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
void zmk_2g4_crypto_session_start(void);
int zmk_2g4_crypto_encrypt(uint8_t *data, size_t len, size_t buf_size);
int zmk_2g4_crypto_decrypt(uint8_t *data, size_t len);

int zmk_2g4_crypto_set_paired_key(const uint8_t key[16]);
int zmk_2g4_crypto_clear_paired_key(void);
bool zmk_2g4_crypto_uses_factory_key(void);
bool zmk_2g4_crypto_has_paired_key(void);
uint32_t zmk_2g4_crypto_key_id(void);
