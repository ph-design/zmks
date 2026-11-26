/*
 * Copyright (c) 2026 Team PHDesign
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZMK_2G4_SESSION_CHALLENGE 0xF0
#define ZMK_2G4_SESSION_RESPONSE 0xF1
#define ZMK_2G4_SESSION_CONFIRM 0xF2
#define ZMK_2G4_SESSION_READY 0xF3
#define ZMK_2G4_SESSION_HELLO 0xF4
#define ZMK_2G4_SESSION_READY_ACK 0xF5
#define ZMK_2G4_SESSION_NONCE_SIZE 12
#define ZMK_2G4_SESSION_FRAME_SIZE 17

enum zmk_2g4_session_state {
    ZMK_2G4_SESSION_WAIT_PEER,
    ZMK_2G4_SESSION_WAIT_RESPONSE,
    ZMK_2G4_SESSION_WAIT_CONFIRM,
    ZMK_2G4_SESSION_WAIT_READY,
    ZMK_2G4_SESSION_SEND_READY,
    ZMK_2G4_SESSION_WAIT_READY_ACK,
    ZMK_2G4_SESSION_ESTABLISHED,
};

/* The crypto state mutex owns this state. Session packets are still AES-CCM protected. */
struct zmk_2g4_session {
    enum zmk_2g4_session_state state;
    uint32_t local_epoch;
    uint32_t peer_epoch;
    uint8_t nonce[ZMK_2G4_SESSION_NONCE_SIZE];
    bool ready_ack_pending;
};

void zmk_2g4_session_init(struct zmk_2g4_session *session, uint32_t epoch);
bool zmk_2g4_session_ready(const struct zmk_2g4_session *session);
int zmk_2g4_session_receive(struct zmk_2g4_session *session, const uint8_t *data, size_t len,
                            uint32_t peer_epoch, bool *fresh);
int zmk_2g4_session_prepare(const struct zmk_2g4_session *session, uint8_t *data, size_t size);
void zmk_2g4_session_sent(struct zmk_2g4_session *session, uint8_t type);
