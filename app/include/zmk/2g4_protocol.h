/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

// Device to Dongle
#define ZMK_2G4_MSG_KEYBOARD_REPORT 0x01
#define ZMK_2G4_MSG_CONSUMER_REPORT 0x02
#define ZMK_2G4_MSG_MOUSE_REPORT 0x03
#define ZMK_2G4_MSG_SYSTEM_REPORT 0x04
#define ZMK_2G4_MSG_STUDIO_RPC_TX 0x10
#define ZMK_2G4_MSG_KEEP_ALIVE 0xFE

// Dongle to Device
#define ZMK_2G4_ACK_LED_INDICATORS 0x01
#define ZMK_2G4_ACK_RESOLUTION_MULTIPLIER 0x02
#define ZMK_2G4_ACK_HOST_CONNECTED 0x03
#define ZMK_2G4_ACK_STUDIO_RPC_RX 0x10

// Frequency agility channel table
// Spread across 2.4GHz band, avoiding WiFi channels 1(2412)/6(2437)/11(2462)
// ch2=2402, ch25=2425, ch50=2450, ch73=2473, ch95=2495
#define ZMK_2G4_CHANNEL_COUNT 5
static const uint8_t zmk_2g4_channels[ZMK_2G4_CHANNEL_COUNT] = {2, 25, 50, 73, 95};
