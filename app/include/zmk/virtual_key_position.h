/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zmk/matrix.h>
#include <zmk/combos.h>
#include <zmk/input_listeners.h>
#include <zmk/sensors.h>

/**
 * Gets the virtual key position to use for the sensor with the given index.
 */
#define ZMK_VIRTUAL_KEY_POSITION_SENSOR(index) (ZMK_KEYMAP_LEN + (index))

/**
 * Gets the sensor number from the virtual key position.
 */
#define ZMK_SENSOR_POSITION_FROM_VIRTUAL_KEY_POSITION(vkp) ((vkp) - ZMK_KEYMAP_LEN)

/**
 * Gets the virtual key position to use for the combo with the given index.
 */
#define ZMK_VIRTUAL_KEY_POSITION_COMBO(index)                                                      \
    (ZMK_VIRTUAL_KEY_POSITION_SENSOR(ZMK_KEYMAP_SENSORS_LEN) + (index))

#define ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(listener_index, processor_index)         \
    (ZMK_VIRTUAL_KEY_POSITION_COMBO(ZMK_COMBOS_LEN) +                                              \
     (ZMK_INPUT_LISTENERS_LEN * (processor_index)) + (listener_index))

/* processor positions grow with LISTENERS_LEN per processor index; motion sits
 * past all of them, assuming at most this many processors per listener */
#define ZMK_MOTION_VKP_MAX_PROCESSORS_PER_LISTENER 16
#define ZMK_VIRTUAL_KEY_POSITION_MOTION(slot)                                                      \
    (ZMK_VIRTUAL_KEY_POSITION_COMBO(ZMK_COMBOS_LEN) +                                              \
     (ZMK_INPUT_LISTENERS_LEN * ZMK_MOTION_VKP_MAX_PROCESSORS_PER_LISTENER) + (slot))
