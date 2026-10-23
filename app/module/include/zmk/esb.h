/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

enum zmk_esb_mode {
    ZMK_ESB_MODE_PTX,
    ZMK_ESB_MODE_PRX,
};

enum zmk_esb_bitrate {
    ZMK_ESB_BITRATE_1MBPS,
    ZMK_ESB_BITRATE_2MBPS,
};

enum zmk_esb_crc {
    ZMK_ESB_CRC_OFF,
    ZMK_ESB_CRC_8BIT,
    ZMK_ESB_CRC_16BIT,
};

enum zmk_esb_tx_mode {
    ZMK_ESB_TXMODE_AUTO,
    ZMK_ESB_TXMODE_MANUAL,
};

enum zmk_esb_event_id {
    ZMK_ESB_EVENT_TX_SUCCESS,
    ZMK_ESB_EVENT_TX_FAILED,
    ZMK_ESB_EVENT_RX_RECEIVED,
};

struct zmk_esb_event {
    enum zmk_esb_event_id evt_id;
    uint32_t tx_attempts;
};

typedef void (*zmk_esb_event_handler_t)(const struct zmk_esb_event *event);

struct zmk_esb_payload {
    uint8_t length;
    uint8_t pipe;
    int8_t rssi;
    uint8_t pid;
    bool noack;
    uint8_t data[CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH];
};

struct zmk_esb_config {
    enum zmk_esb_mode mode;
    enum zmk_esb_bitrate bitrate;
    enum zmk_esb_crc crc;
    enum zmk_esb_tx_mode tx_mode;
    zmk_esb_event_handler_t event_handler;
    bool selective_auto_ack;
    uint16_t retransmit_delay;
    uint16_t retransmit_count;
};

int zmk_esb_init(const struct zmk_esb_config *config);
void zmk_esb_disable(void);
bool zmk_esb_is_idle(void);

int zmk_esb_write_payload(const struct zmk_esb_payload *payload);
int zmk_esb_read_rx_payload(struct zmk_esb_payload *payload);

int zmk_esb_start_tx(void);
int zmk_esb_start_rx(void);
int zmk_esb_stop_rx(void);

int zmk_esb_flush_tx(void);
int zmk_esb_flush_rx(void);

int zmk_esb_set_base_address_0(const uint8_t *addr);
int zmk_esb_set_base_address_1(const uint8_t *addr);
int zmk_esb_set_prefixes(const uint8_t *prefixes, uint8_t num_pipes);
int zmk_esb_set_rf_channel(uint32_t channel);
int zmk_esb_set_tx_power(int8_t tx_power_dbm);
