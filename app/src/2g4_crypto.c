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
#include <hal/nrf_ccm.h>
#include <zmk/2g4_crypto.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define AES_KEY_SIZE 16
#define SESSION_ID_SIZE 4
#define CTR_SIZE 4
#define HEADER_SIZE (SESSION_ID_SIZE + CTR_SIZE)
#define MIC_SIZE 4
#define OVERHEAD (HEADER_SIZE + MIC_SIZE)

#define CCM_DIR_DATA 1
#define CCM_PDU_HDR 3
#define CCM_MAX_PAYLOAD 23
#define CCM_SCRATCH_SIZE 43

static const char *key_hex = CONFIG_ZMK_2G4_AES_KEY;
static uint8_t aes_key[AES_KEY_SIZE];
static bool key_valid;
static bool ccm_ok;
static uint32_t tx_session_id;
static uint32_t tx_counter;
static uint32_t rx_session_id;
static uint32_t rx_counter;
static bool rx_session_init;

static struct k_spinlock ccm_lock;

struct ccm_cnf {
    uint8_t key[AES_KEY_SIZE];
    uint64_t counter;
    uint8_t direction;
    uint8_t iv[8];
} __packed;

static struct ccm_cnf cnf __aligned(4);
static uint8_t ccm_in[CCM_PDU_HDR + CCM_MAX_PAYLOAD + MIC_SIZE] __aligned(4);
static uint8_t ccm_out[CCM_PDU_HDR + CCM_MAX_PAYLOAD + MIC_SIZE] __aligned(4);
static uint8_t ccm_scratch[CCM_SCRATCH_SIZE] __aligned(4);

static int parse_hex_key(void) {
    if (strlen(key_hex) != 32) {
        return -EINVAL;
    }
    if (hex2bin(key_hex, 32, aes_key, AES_KEY_SIZE) != AES_KEY_SIZE) {
        return -EINVAL;
    }
    return 0;
}

static void ccm_set_cnf(const uint8_t key[AES_KEY_SIZE], uint32_t session_id, uint32_t counter,
                        uint8_t direction) {
    memcpy(cnf.key, key, AES_KEY_SIZE);
    cnf.counter = counter;
    cnf.direction = direction & 0x01;
    memset(cnf.iv, 0, sizeof(cnf.iv));
    sys_put_le32(session_id, cnf.iv);
}

static int ccm_run(bool encrypt) {
    NRF_CCM->ENABLE = CCM_ENABLE_ENABLE_Disabled;
    NRF_CCM->ENABLE = CCM_ENABLE_ENABLE_Enabled;
    NRF_CCM->MODE = ((uint32_t)(encrypt ? CCM_MODE_MODE_Encryption : CCM_MODE_MODE_Decryption)
                     << CCM_MODE_MODE_Pos) |
                    ((uint32_t)CCM_MODE_DATARATE_2Mbit << CCM_MODE_DATARATE_Pos) |
                    ((uint32_t)CCM_MODE_LENGTH_Default << CCM_MODE_LENGTH_Pos);
    NRF_CCM->CNFPTR = (uint32_t)&cnf;
    NRF_CCM->INPTR = (uint32_t)ccm_in;
    NRF_CCM->OUTPTR = (uint32_t)ccm_out;
    NRF_CCM->SCRATCHPTR = (uint32_t)ccm_scratch;
    NRF_CCM->SHORTS = 0;

    nrf_ccm_event_clear(NRF_CCM, NRF_CCM_EVENT_ENDKSGEN);
    nrf_ccm_event_clear(NRF_CCM, NRF_CCM_EVENT_ERROR);
    nrf_ccm_task_trigger(NRF_CCM, NRF_CCM_TASK_KSGEN);
    if (!WAIT_FOR(nrf_ccm_event_check(NRF_CCM, NRF_CCM_EVENT_ENDKSGEN) ||
                      nrf_ccm_event_check(NRF_CCM, NRF_CCM_EVENT_ERROR),
                  1000, k_busy_wait(1))) {
        return -ETIMEDOUT;
    }
    if (nrf_ccm_event_check(NRF_CCM, NRF_CCM_EVENT_ERROR)) {
        return -EIO;
    }

    nrf_ccm_event_clear(NRF_CCM, NRF_CCM_EVENT_ENDCRYPT);
    nrf_ccm_event_clear(NRF_CCM, NRF_CCM_EVENT_ERROR);
    nrf_ccm_task_trigger(NRF_CCM, NRF_CCM_TASK_CRYPT);
    if (!WAIT_FOR(nrf_ccm_event_check(NRF_CCM, NRF_CCM_EVENT_ENDCRYPT) ||
                      nrf_ccm_event_check(NRF_CCM, NRF_CCM_EVENT_ERROR),
                  1000, k_busy_wait(1))) {
        return -ETIMEDOUT;
    }
    if (nrf_ccm_event_check(NRF_CCM, NRF_CCM_EVENT_ERROR)) {
        return -EIO;
    }

    return 0;
}

static int ccm_encrypt_payload(const uint8_t key[AES_KEY_SIZE], uint32_t session_id,
                               uint32_t counter, uint8_t direction, const uint8_t *pt,
                               size_t pt_len, uint8_t *ct_mic_out) {
    if (pt_len > CCM_MAX_PAYLOAD) {
        return -ENOMEM;
    }

    k_spinlock_key_t lock = k_spin_lock(&ccm_lock);
    ccm_set_cnf(key, session_id, counter, direction);
    ccm_in[0] = 0;
    ccm_in[1] = (uint8_t)pt_len;
    ccm_in[2] = 0;
    memcpy(&ccm_in[CCM_PDU_HDR], pt, pt_len);

    int ret = ccm_run(true);
    if (ret == 0) {
        memcpy(ct_mic_out, &ccm_out[CCM_PDU_HDR], pt_len + MIC_SIZE);
    }
    k_spin_unlock(&ccm_lock, lock);
    return ret;
}

static int ccm_decrypt_payload(const uint8_t key[AES_KEY_SIZE], uint32_t session_id,
                               uint32_t counter, uint8_t direction, const uint8_t *ct_mic,
                               size_t ct_len, uint8_t *pt_out, bool *mic_ok) {
    if (ct_len > CCM_MAX_PAYLOAD) {
        return -EINVAL;
    }

    k_spinlock_key_t lock = k_spin_lock(&ccm_lock);
    ccm_set_cnf(key, session_id, counter, direction);
    ccm_in[0] = 0;
    ccm_in[1] = (uint8_t)(ct_len + MIC_SIZE);
    ccm_in[2] = 0;
    memcpy(&ccm_in[CCM_PDU_HDR], ct_mic, ct_len + MIC_SIZE);

    int ret = ccm_run(false);
    if (ret == 0) {
        *mic_ok = nrf_ccm_micstatus_get(NRF_CCM);
        if (*mic_ok) {
            memcpy(pt_out, &ccm_out[CCM_PDU_HDR], ct_len);
        }
    }
    k_spin_unlock(&ccm_lock, lock);
    return ret;
}

static bool ccm_self_test(void) {
    static const uint8_t tk[AES_KEY_SIZE] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                             0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    static const uint8_t pt[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t ct_mic[8 + MIC_SIZE];
    uint8_t out[8];
    bool ok = false;

    if (ccm_encrypt_payload(tk, 0x12345678u, 1u, CCM_DIR_DATA, pt, sizeof(pt), ct_mic)) {
        return false;
    }
    if (ccm_decrypt_payload(tk, 0x12345678u, 1u, CCM_DIR_DATA, ct_mic, sizeof(pt), out, &ok)) {
        return false;
    }
    if (!ok || memcmp(out, pt, sizeof(pt)) != 0) {
        return false;
    }

    ct_mic[0] ^= 0x01;
    if (ccm_decrypt_payload(tk, 0x12345678u, 1u, CCM_DIR_DATA, ct_mic, sizeof(pt), out, &ok)) {
        return false;
    }
    if (ok) {
        return false;
    }
    return true;
}

static int zmk_2g4_crypto_init(void) {
    if (strlen(key_hex) == 0) {
        key_valid = false;
        return 0;
    }
    if (parse_hex_key()) {
        LOG_ERR("Invalid 2.4G AES key (need 32 hex chars)");
        key_valid = false;
        return 0;
    }
    key_valid = true;

    ccm_ok = ccm_self_test();
    if (!ccm_ok) {
        LOG_ERR("2.4G CCM self-test failed; encryption disabled");
        return 0;
    }

    do {
        tx_session_id = sys_rand32_get();
    } while (tx_session_id == 0);

    LOG_INF("2.4G AES-CCM enabled");
    return 0;
}

SYS_INIT(zmk_2g4_crypto_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

bool zmk_2g4_crypto_enabled(void) { return key_valid && ccm_ok; }

uint32_t zmk_2g4_crypto_tx_session_id(void) { return tx_session_id; }

int zmk_2g4_crypto_encrypt(uint8_t *data, size_t len, size_t buf_size) {
    __ASSERT_NO_MSG(data != NULL);

    if (!key_valid) {
        return (int)len;
    }
    if (!ccm_ok) {
        return -EIO;
    }
    if (len + OVERHEAD > buf_size) {
        return -ENOMEM;
    }
    if (tx_counter == 0xFFFFFFFFu) {
        LOG_ERR("TX counter exhausted, re-key required");
        return -ERANGE;
    }

    uint32_t ctr = tx_counter++;
    uint8_t ct_mic[CCM_MAX_PAYLOAD + MIC_SIZE];

    int ret = ccm_encrypt_payload(aes_key, tx_session_id, ctr, CCM_DIR_DATA, data, len, ct_mic);
    if (ret) {
        tx_counter--;
        return ret;
    }

    sys_put_le32(tx_session_id, data);
    sys_put_le32(ctr, data + SESSION_ID_SIZE);
    memcpy(data + HEADER_SIZE, ct_mic, len + MIC_SIZE);

    return (int)(len + OVERHEAD);
}

int zmk_2g4_crypto_decrypt(uint8_t *data, size_t len) {
    __ASSERT_NO_MSG(data != NULL);

    if (!key_valid) {
        return (int)len;
    }
    if (!ccm_ok) {
        return -EIO;
    }
    if (len <= OVERHEAD) {
        return -EINVAL;
    }

    uint32_t session_id = sys_get_le32(data);
    uint32_t ctr = sys_get_le32(data + SESSION_ID_SIZE);
    size_t ct_len = len - OVERHEAD;

    uint8_t pt[CCM_MAX_PAYLOAD];
    bool mic_ok = false;
    int ret = ccm_decrypt_payload(aes_key, session_id, ctr, CCM_DIR_DATA, data + HEADER_SIZE,
                                  ct_len, pt, &mic_ok);
    if (ret) {
        return ret;
    }
    if (!mic_ok) {
        return -EBADMSG;
    }

    bool new_session = !rx_session_init || (session_id != rx_session_id);
    if (!new_session && (int32_t)(ctr - rx_counter) <= 0) {
        return -EACCES;
    }

    memcpy(data, pt, ct_len);

    if (new_session) {
        rx_session_id = session_id;
        rx_session_init = true;
    }
    rx_counter = ctr;

    return (int)ct_len;
}
