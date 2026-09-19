/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/random/random.h>
#include <pb_encode.h>
#include <zmk/studio/rpc.h>
#include <zmk/2g4.h>
#include <zmk/2g4_crypto.h>

ZMK_RPC_SUBSYSTEM(pairing)

#define PAIRING_RESPONSE(type, ...) ZMK_RPC_RESPONSE(pairing, type, __VA_ARGS__)

zmk_studio_Response get_pair_state(const zmk_studio_Request *req) {
    zmk_pairing_GetPairStateResponse resp = zmk_pairing_GetPairStateResponse_init_zero;

    struct zmk_2g4_addr addr;
    zmk_2g4_addr_get(&addr);

    resp.uses_factory_key = zmk_2g4_crypto_uses_factory_key();
    resp.key_id = zmk_2g4_crypto_key_id();
    resp.addr_paired = zmk_2g4_addr_is_paired();
    memcpy(resp.rf_addr.bytes, addr.base, 4);
    resp.rf_addr.bytes[4] = addr.prefix;
    resp.rf_addr.size = 5;
    resp.rf_channel = addr.rf_channel;

    return PAIRING_RESPONSE(get_pair_state, resp);
}

zmk_studio_Response generate_and_pair(const zmk_studio_Request *req) {
    if (!zmk_2g4_crypto_enabled()) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    uint8_t key[16];
    struct zmk_2g4_addr addr;

    sys_rand_get(key, sizeof(key));
    sys_rand_get(addr.base, sizeof(addr.base));
    sys_rand_get(&addr.prefix, 1);
    addr.rf_channel = (uint8_t)(sys_rand32_get() % 101);

    int err = zmk_2g4_crypto_set_paired_key(key);
    if (!err) {
        err = zmk_2g4_addr_set_paired(&addr);
    }
    if (err) {
        LOG_ERR("pairing persist failed: %d", err);
        // roll back to a consistent factory state instead of a half-paired identity
        zmk_2g4_crypto_clear_paired_key();
        zmk_2g4_addr_clear_paired();
        memset(key, 0, sizeof(key));
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_2g4_stop();
    int ret = zmk_2g4_start();
    LOG_INF("paired, transport restarted: %d (key_id=%08x ch=%u)", ret,
            zmk_2g4_crypto_key_id(), addr.rf_channel);

    zmk_pairing_GenerateAndPairResponse resp = zmk_pairing_GenerateAndPairResponse_init_zero;
    resp.key_id = zmk_2g4_crypto_key_id();
    memcpy(resp.key.bytes, key, sizeof(key));
    resp.key.size = sizeof(key);
    memcpy(resp.rf_addr.bytes, addr.base, 4);
    resp.rf_addr.bytes[4] = addr.prefix;
    resp.rf_addr.size = 5;
    resp.rf_channel = addr.rf_channel;

    memset(key, 0, sizeof(key));
    return PAIRING_RESPONSE(generate_and_pair, resp);
}

zmk_studio_Response clear_pairing(const zmk_studio_Request *req) {
    int err = zmk_2g4_crypto_clear_paired_key();
    err |= zmk_2g4_addr_clear_paired();
    if (err && err != -ENOENT) {
        LOG_ERR("clear pairing failed: %d", err);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_2g4_stop();
    int ret = zmk_2g4_start();
    LOG_INF("pairing cleared, transport restarted: %d", ret);

    zmk_pairing_ClearPairingResponse resp = zmk_pairing_ClearPairingResponse_init_zero;
    return PAIRING_RESPONSE(clear_pairing, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(pairing, get_pair_state, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(pairing, generate_and_pair, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(pairing, clear_pairing, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
