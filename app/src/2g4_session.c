/*
 * Copyright (c) 2026 Team PHDesign
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>

#include <zmk/2g4_session.h>

void zmk_2g4_session_init(struct zmk_2g4_session *session, uint32_t epoch) {
    memset(session, 0, sizeof(*session));
    session->local_epoch = epoch;
}

bool zmk_2g4_session_ready(const struct zmk_2g4_session *session) {
    return session->state == ZMK_2G4_SESSION_ESTABLISHED;
}

static bool is_control(uint8_t type) {
    return type >= ZMK_2G4_SESSION_CHALLENGE && type <= ZMK_2G4_SESSION_READY_ACK;
}

static bool matches(const struct zmk_2g4_session *session, const uint8_t *data, size_t len,
                    uint32_t keyboard_epoch) {
    return len == ZMK_2G4_SESSION_FRAME_SIZE && sys_get_le32(data + 1) == keyboard_epoch &&
           !memcmp(data + 5, session->nonce, sizeof(session->nonce));
}

int zmk_2g4_session_receive(struct zmk_2g4_session *session, const uint8_t *data, size_t len,
                            uint32_t peer_epoch, bool *fresh) {
    *fresh = false;
    if (!len || !peer_epoch || !session->local_epoch) {
        return -EINVAL;
    }
    if (session->peer_epoch && peer_epoch != session->peer_epoch &&
        peer_epoch < session->peer_epoch) {
        return -EACCES;
    }

#if IS_ENABLED(CONFIG_ZMK_2G4_DONGLE)
    if (!is_control(data[0]) || data[0] == ZMK_2G4_SESSION_HELLO) {
        if (zmk_2g4_session_ready(session) && peer_epoch == session->peer_epoch &&
            data[0] != ZMK_2G4_SESSION_HELLO) {
            return 1;
        }
        if (!session->peer_epoch || peer_epoch != session->peer_epoch ||
            session->state == ZMK_2G4_SESSION_ESTABLISHED) {
            uint8_t nonce[ZMK_2G4_SESSION_NONCE_SIZE];
            int err = sys_csrand_get(nonce, sizeof(nonce));
            if (err) {
                zmk_2g4_session_init(session, session->local_epoch);
                return err;
            }
            memcpy(session->nonce, nonce, sizeof(nonce));
            session->peer_epoch = peer_epoch;
            session->state = ZMK_2G4_SESSION_WAIT_RESPONSE;
            session->ready_ack_pending = false;
        }
        return -EAGAIN;
    }
    if (peer_epoch != session->peer_epoch || !matches(session, data, len, session->peer_epoch)) {
        return -EACCES;
    }
    if (data[0] == ZMK_2G4_SESSION_RESPONSE && (session->state == ZMK_2G4_SESSION_WAIT_RESPONSE ||
                                                session->state == ZMK_2G4_SESSION_WAIT_READY)) {
        session->state = ZMK_2G4_SESSION_WAIT_READY;
        return 0;
    }
    if (data[0] == ZMK_2G4_SESSION_READY && (session->state == ZMK_2G4_SESSION_WAIT_READY ||
                                             session->state == ZMK_2G4_SESSION_ESTABLISHED)) {
        *fresh = session->state == ZMK_2G4_SESSION_WAIT_READY;
        session->state = ZMK_2G4_SESSION_ESTABLISHED;
        session->ready_ack_pending = true;
        return 0;
    }
#else
    if (data[0] == ZMK_2G4_SESSION_CHALLENGE) {
        if (len != ZMK_2G4_SESSION_FRAME_SIZE || sys_get_le32(data + 1) != session->local_epoch) {
            return -EACCES;
        }
        if (peer_epoch != session->peer_epoch ||
            memcmp(session->nonce, data + 5, sizeof(session->nonce))) {
            memcpy(session->nonce, data + 5, sizeof(session->nonce));
            session->peer_epoch = peer_epoch;
            session->state = ZMK_2G4_SESSION_WAIT_CONFIRM;
        }
        return 0;
    }
    if (data[0] == ZMK_2G4_SESSION_CONFIRM && peer_epoch == session->peer_epoch &&
        matches(session, data, len, session->local_epoch) &&
        (session->state == ZMK_2G4_SESSION_WAIT_CONFIRM ||
         session->state == ZMK_2G4_SESSION_SEND_READY ||
         session->state == ZMK_2G4_SESSION_WAIT_READY_ACK ||
         session->state == ZMK_2G4_SESSION_ESTABLISHED)) {
        if (!zmk_2g4_session_ready(session)) {
            session->state = ZMK_2G4_SESSION_SEND_READY;
            *fresh = true;
        }
        return 0;
    }
    if (data[0] == ZMK_2G4_SESSION_READY_ACK && peer_epoch == session->peer_epoch &&
        matches(session, data, len, session->local_epoch) &&
        (session->state == ZMK_2G4_SESSION_WAIT_READY_ACK ||
         zmk_2g4_session_ready(session))) {
        session->state = ZMK_2G4_SESSION_ESTABLISHED;
        return 0;
    }
    if (!is_control(data[0]) && peer_epoch == session->peer_epoch) {
        // A legacy receiver can only send authenticated data after accepting READY.
        if (session->state == ZMK_2G4_SESSION_WAIT_READY_ACK) {
            session->state = ZMK_2G4_SESSION_ESTABLISHED;
        }
        if (zmk_2g4_session_ready(session)) {
            return 1;
        }
    }
#endif
    return -EACCES;
}

int zmk_2g4_session_prepare(const struct zmk_2g4_session *session, uint8_t *data, size_t size) {
    uint8_t type;
#if IS_ENABLED(CONFIG_ZMK_2G4_DONGLE)
    if (session->state == ZMK_2G4_SESSION_WAIT_RESPONSE) {
        type = ZMK_2G4_SESSION_CHALLENGE;
    } else if (session->state == ZMK_2G4_SESSION_WAIT_READY) {
        type = ZMK_2G4_SESSION_CONFIRM;
    } else if (session->ready_ack_pending) {
        type = ZMK_2G4_SESSION_READY_ACK;
    } else {
        return 0;
    }
    uint32_t keyboard_epoch = session->peer_epoch;
#else
    if (session->state == ZMK_2G4_SESSION_WAIT_PEER) {
        if (!size) {
            return -ENOMEM;
        }
        data[0] = ZMK_2G4_SESSION_HELLO;
        return 1;
    } else if (session->state == ZMK_2G4_SESSION_WAIT_CONFIRM) {
        type = ZMK_2G4_SESSION_RESPONSE;
    } else if (session->state == ZMK_2G4_SESSION_SEND_READY ||
               session->state == ZMK_2G4_SESSION_WAIT_READY_ACK) {
        type = ZMK_2G4_SESSION_READY;
    } else {
        return 0;
    }
    uint32_t keyboard_epoch = session->local_epoch;
#endif
    if (size < ZMK_2G4_SESSION_FRAME_SIZE) {
        return -ENOMEM;
    }
    data[0] = type;
    sys_put_le32(keyboard_epoch, data + 1);
    memcpy(data + 5, session->nonce, sizeof(session->nonce));
    return ZMK_2G4_SESSION_FRAME_SIZE;
}

void zmk_2g4_session_sent(struct zmk_2g4_session *session, uint8_t type) {
#if IS_ENABLED(CONFIG_ZMK_2G4_DONGLE)
    if (type == ZMK_2G4_SESSION_READY_ACK) {
        session->ready_ack_pending = false;
    }
#else
    if (type == ZMK_2G4_SESSION_READY && session->state == ZMK_2G4_SESSION_SEND_READY) {
        session->state = ZMK_2G4_SESSION_WAIT_READY_ACK;
    }
#endif
}
