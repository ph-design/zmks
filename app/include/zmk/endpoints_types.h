/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * The method by which data is sent.
 */
enum zmk_transport {
    ZMK_TRANSPORT_USB,
    ZMK_TRANSPORT_BLE,
#if IS_ENABLED(CONFIG_ZMK_2G4)
    ZMK_TRANSPORT_2G4,
#endif
};

/**
 * Configuration to select an endpoint on ZMK_TRANSPORT_USB.
 */
struct zmk_transport_usb_data {};

/**
 * Configuration to select an endpoint on ZMK_TRANSPORT_BLE.
 */
struct zmk_transport_ble_data {
    int profile_index;
};

#if IS_ENABLED(CONFIG_ZMK_2G4)
struct zmk_transport_2g4_data {};
#endif

/**
 * A specific endpoint to which data may be sent.
 */
struct zmk_endpoint_instance {
    enum zmk_transport transport;
    union {
        struct zmk_transport_usb_data usb; // ZMK_TRANSPORT_USB
        struct zmk_transport_ble_data ble; // ZMK_TRANSPORT_BLE
#if IS_ENABLED(CONFIG_ZMK_2G4)
        struct zmk_transport_2g4_data _2g4; // ZMK_TRANSPORT_2G4
#endif
    };
};
