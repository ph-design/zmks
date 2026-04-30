/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <hal/nrf_ecb.h>
#include <zmk/2g4_crypto.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define AES_BLOCK_SIZE 16
#define SESSION_ID_SIZE 4
#define CTR_SIZE 4
#define HEADER_SIZE (SESSION_ID_SIZE + CTR_SIZE)

static const char *key_hex = CONFIG_ZMK_2G4_AES_KEY;
static bool key_valid;
static uint32_t tx_session_id;
static uint32_t tx_counter;
static uint32_t rx_session_id;
static uint32_t rx_counter;
static bool rx_session_init;

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

    /* Random per-boot session id; 0 reserved as "unset" */
    do {
        tx_session_id = sys_rand32_get();
    } while (tx_session_id == 0);

    LOG_INF("2.4G AES-128 enabled (tx_session=0x%08x)", tx_session_id);
    return 0;
}

SYS_INIT(zmk_2g4_crypto_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

bool zmk_2g4_crypto_enabled(void) { return key_valid; }

uint32_t zmk_2g4_crypto_tx_session_id(void) { return tx_session_id; }

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

static void aes_ctr_xor(uint32_t session_id, uint32_t counter, uint8_t *data, size_t len) {
    uint8_t nonce[AES_BLOCK_SIZE] = {0};
    uint8_t ks[AES_BLOCK_SIZE];

    sys_put_le32(session_id, nonce);
    sys_put_le32(counter, nonce + 4);

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
    if (len + HEADER_SIZE > buf_size) {
        return -ENOMEM;
    }

    uint32_t ctr = tx_counter++;

    memmove(data + HEADER_SIZE, data, len);
    sys_put_le32(tx_session_id, data);
    sys_put_le32(ctr, data + SESSION_ID_SIZE);
    aes_ctr_xor(tx_session_id, ctr, data + HEADER_SIZE, len);

    return (int)(len + HEADER_SIZE);
}

int zmk_2g4_crypto_decrypt(uint8_t *data, size_t len) {
    if (!key_valid) {
        return (int)len;
    }
    if (len <= HEADER_SIZE) {
        return -EINVAL;
    }

    uint32_t session_id = sys_get_le32(data);
    uint32_t ctr = sys_get_le32(data + SESSION_ID_SIZE);

    bool new_session = !rx_session_init || (session_id != rx_session_id);

    if (!new_session && ctr <= rx_counter) {
        LOG_WRN("2.4G replay: got %u, last %u (session 0x%08x)", ctr, rx_counter, session_id);
        return -EACCES;
    }

    size_t payload_len = len - HEADER_SIZE;
    aes_ctr_xor(session_id, ctr, data + HEADER_SIZE, payload_len);
    memmove(data, data + HEADER_SIZE, payload_len);

    if (new_session) {
        LOG_INF("2.4G new session 0x%08x (was 0x%08x), rx_counter %u -> %u",
                session_id, rx_session_id, rx_counter, ctr);
        rx_session_id = session_id;
        rx_session_init = true;
    }
    rx_counter = ctr;

    return (int)payload_len;
}
