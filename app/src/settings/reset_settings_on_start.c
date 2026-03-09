/*
 * Copyright (c) 2023 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/sys/reboot.h>
#include <dt-bindings/zmk/reset.h>

#include <zmk/settings.h>

static int zmk_settings_erase_and_reboot_to_bootloader(void) {
    int ret = zmk_settings_erase();
    if (ret == 0) {
        // Reboot into UF2 bootloader so user can flash normal firmware
        sys_reboot(RST_UF2);
    }
    return ret;
}

// Reset after the kernel is initialized but before any application code to
// ensure settings are cleared before anything tries to use them.
SYS_INIT(zmk_settings_erase_and_reboot_to_bootloader, POST_KERNEL,
         CONFIG_ZMK_SETTINGS_RESET_ON_START_INIT_PRIORITY);
