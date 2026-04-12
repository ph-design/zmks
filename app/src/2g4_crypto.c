/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <hal/nrf_ecb.h>
#include <zmk/2g4_crypto.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define AES_BLOCK_SIZE 16
#define CTR_PREFIX_SIZE 4

static const char *key_hex = CONFIG_ZMK_2G4_AES_KEY;
static bool key_valid;
static uint32_t tx_counter;
static uint32_t rx_counter;

/* ECB block layout: key(16) | plaintext(16) | ciphertext(16), 4-byte aligned for NRF_ECB */
static uint8_t ecb_block[48] __aligned(4);

static int parse_hex_key(void) {
    if (strlen(key_hex) != 32) {
        return -EINVAL;
    }
    if (hex2bin(key_hex, 32, ecb_block, AES_BLOCK_SIZE) != AES_BLOCK_SIZE) {
        return -EINVAL;
    }
    return 0;
}

static int zmk_2g4_crypto_init(void) {
    if (strlen(key_hex) == 0) {
        key_valid = false;
        return 0;
    }
    if (parse_hex_key()) {
        LOG_ERR("Invalid 2.4G AES key (need 32 hex chars)");
        key_valid = false;
        return -EINVAL;
    }
    key_valid = true;
    LOG_INF("2.4G AES-128 encryption enabled");
    return 0;
}

SYS_INIT(zmk_2g4_crypto_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

bool zmk_2g4_crypto_enabled(void) { return key_valid; }

static void ecb_encrypt(const uint8_t nonce[16], uint8_t out[16]) {
    memcpy(&ecb_block[16], nonce, AES_BLOCK_SIZE);

    NRF_ECB->ECBDATAPTR = (uint32_t)ecb_block;
    NRF_ECB->EVENTS_ENDECB = 0;
    NRF_ECB->EVENTS_ERRORECB = 0;
    NRF_ECB->TASKS_STARTECB = 1;

    while (!NRF_ECB->EVENTS_ENDECB && !NRF_ECB->EVENTS_ERRORECB) {
    }

    memcpy(out, &ecb_block[32], AES_BLOCK_SIZE);
}

static void aes_ctr_xor(uint32_t counter, uint8_t *data, size_t len) {
    uint8_t nonce[AES_BLOCK_SIZE] = {0};
    uint8_t ks[AES_BLOCK_SIZE];

    sys_put_le32(counter, nonce);

    /* Block 0 */
    ecb_encrypt(nonce, ks);
    size_t n = MIN(len, AES_BLOCK_SIZE);
    for (size_t i = 0; i < n; i++) {
        data[i] ^= ks[i];
    }

    if (len <= AES_BLOCK_SIZE) {
        return;
    }

    /* Block 1 */
    nonce[15] = 1;
    ecb_encrypt(nonce, ks);
    for (size_t i = 0; i < len - AES_BLOCK_SIZE; i++) {
        data[AES_BLOCK_SIZE + i] ^= ks[i];
    }
}

int zmk_2g4_crypto_encrypt(uint8_t *data, size_t len, size_t buf_size) {
    if (!key_valid) {
        return (int)len;
    }
    if (len + CTR_PREFIX_SIZE > buf_size) {
        return -ENOMEM;
    }

    uint32_t ctr = tx_counter++;

    memmove(data + CTR_PREFIX_SIZE, data, len);
    sys_put_le32(ctr, data);
    aes_ctr_xor(ctr, data + CTR_PREFIX_SIZE, len);

    return (int)(len + CTR_PREFIX_SIZE);
}

int zmk_2g4_crypto_decrypt(uint8_t *data, size_t len) {
    if (!key_valid) {
        return (int)len;
    }
    if (len <= CTR_PREFIX_SIZE) {
        return -EINVAL;
    }

    uint32_t ctr = sys_get_le32(data);

    if (ctr <= rx_counter && rx_counter != 0) {
        LOG_WRN("2.4G replay: got %u, last %u", ctr, rx_counter);
        return -EACCES;
    }
    rx_counter = ctr;

    size_t payload_len = len - CTR_PREFIX_SIZE;
    aes_ctr_xor(ctr, data + CTR_PREFIX_SIZE, payload_len);
    memmove(data, data + CTR_PREFIX_SIZE, payload_len);

    return (int)payload_len;
}
