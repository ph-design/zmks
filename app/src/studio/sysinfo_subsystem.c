/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <version.h>
#include <app_version.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/studio/rpc.h>
#include <zmk/activity.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/physical_layouts.h>

#if IS_ENABLED(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_2G4)
#include <zmk/2g4_crypto.h>
#endif

extern char _flash_used[];
extern char _image_ram_size[];

#if defined(CONFIG_SOC)
#define MCU_NAME CONFIG_SOC
#elif defined(CONFIG_SOC_PART_NUMBER)
#define MCU_NAME CONFIG_SOC_PART_NUMBER
#else
#define MCU_NAME CONFIG_BOARD
#endif

ZMK_RPC_SUBSYSTEM(sysinfo)

#define SYSINFO_RESPONSE(type, ...) ZMK_RPC_RESPONSE(sysinfo, type, __VA_ARGS__)

// Copy and NUL-terminate a string in a fixed-size nanopb field.
static void copy_str(char *dst, size_t len, const char *src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }

    snprintk(dst, len, "%s", src);
}

// Convert __DATE__ ("Mmm dd yyyy") to ISO format.
static void format_build_date(char *dst, size_t len) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;
    int month = 0;

    for (int i = 0; i < 12; i++) {
        if (d[0] == months[i * 3] && d[1] == months[i * 3 + 1] && d[2] == months[i * 3 + 2]) {
            month = i + 1;
            break;
        }
    }

    int day = (d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0');

    snprintk(dst, len, "%c%c%c%c-%02d-%02d", d[7], d[8], d[9], d[10], month, day);
}

#if IS_ENABLED(CONFIG_HWINFO)
struct reset_cause_entry {
    uint32_t mask;
    const char *text;
};

static const char *reset_cause_text(uint32_t cause) {
    static const struct reset_cause_entry causes[] = {
        {RESET_WATCHDOG, "Watchdog"},
        {RESET_BROWNOUT, "Brownout"},
        {RESET_PIN, "Reset pin"},
        {RESET_SOFTWARE, "Software reset"},
        {RESET_CPU_LOCKUP, "CPU lockup"},
        {RESET_LOW_POWER_WAKE, "Low-power wake"},
        {RESET_SECURITY, "Security violation"},
        {RESET_DEBUG, "Debug reset"},
        {RESET_POR, "Power-on reset"},
        {RESET_PARITY, "Parity error"},
        {RESET_PLL, "PLL failure"},
        {RESET_CLOCK, "Clock failure"},
        {RESET_HARDWARE, "Hardware reset"},
        {RESET_USER, "User reset"},
        {RESET_TEMPERATURE, "Temperature reset"},
    };

    for (size_t i = 0; i < ARRAY_SIZE(causes); i++) {
        if (cause & causes[i].mask) {
            return causes[i].text;
        }
    }

    return cause == 0 ? "No reset recorded" : "Unknown reset";
}
#endif // IS_ENABLED(CONFIG_HWINFO)

// Stack watermarks require both Kconfig options; otherwise report zero.
#if IS_ENABLED(CONFIG_INIT_STACKS) && IS_ENABLED(CONFIG_THREAD_STACK_INFO)
static uint32_t stack_used_pct(const struct k_thread *thread, size_t total) {
    size_t unused = 0;

    if (total == 0 || k_thread_stack_space_get(thread, &unused) != 0) {
        return 0;
    }

    if (unused >= total) {
        return 0;
    }

    return (uint32_t)(((total - unused) * 100U) / total);
}
#endif // IS_ENABLED(CONFIG_INIT_STACKS) && IS_ENABLED(CONFIG_THREAD_STACK_INFO)

static zmk_studio_Response get_identity(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_sysinfo_Identity resp = zmk_sysinfo_Identity_init_zero;

    copy_str(resp.name, sizeof(resp.name), CONFIG_ZMK_KEYBOARD_NAME);
    copy_str(resp.model, sizeof(resp.model), CONFIG_ZMK_DEVICE_MODEL);
    copy_str(resp.manufacturer, sizeof(resp.manufacturer), CONFIG_ZMK_DEVICE_MANUFACTURER);
    copy_str(resp.family, sizeof(resp.family), CONFIG_ZMK_DEVICE_FAMILY);

#if IS_ENABLED(CONFIG_HWINFO)
    uint8_t device_id[sizeof(resp.serial_number.bytes)];
    ssize_t device_id_len = hwinfo_get_device_id(device_id, sizeof(device_id));

    if (device_id_len > 0) {
        resp.serial_number.size =
            (pb_size_t)MIN((size_t)device_id_len, sizeof(resp.serial_number.bytes));
        memcpy(resp.serial_number.bytes, device_id, resp.serial_number.size);
    }
#endif

    return SYSINFO_RESPONSE(identity, resp);
}

static zmk_studio_Response get_firmware(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_sysinfo_FirmwareInfo resp = zmk_sysinfo_FirmwareInfo_init_zero;

    copy_str(resp.zmk_version, sizeof(resp.zmk_version), "v" APP_VERSION_STRING "-lts");
    copy_str(resp.zephyr_version, sizeof(resp.zephyr_version), KERNEL_VERSION_STRING);
    copy_str(resp.build_time, sizeof(resp.build_time), __TIME__);
    format_build_date(resp.build_date, sizeof(resp.build_date));

    // APP_BUILD_VERSION is an unquoted token and must be stringified.
    copy_str(resp.git_hash, sizeof(resp.git_hash), STRINGIFY(APP_BUILD_VERSION));

    copy_str(resp.board, sizeof(resp.board), CONFIG_BOARD);
    copy_str(resp.studio_protocol, sizeof(resp.studio_protocol), "0.3");

    resp.image_size_kb = ((uintptr_t)_flash_used + 1023U) / 1024U;
    resp.ram_used_kb = ((uintptr_t)_image_ram_size + 1023U) / 1024U;

    return SYSINFO_RESPONSE(firmware, resp);
}

static zmk_studio_Response get_hardware(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_sysinfo_HardwareInfo resp = zmk_sysinfo_HardwareInfo_init_zero;

    copy_str(resp.mcu, sizeof(resp.mcu), MCU_NAME);

    // Flash and RAM sizes are already expressed in KiB.
#ifdef CONFIG_FLASH_SIZE
    resp.flash_kb = CONFIG_FLASH_SIZE;
#endif
#ifdef CONFIG_SRAM_SIZE
    resp.ram_kb = CONFIG_SRAM_SIZE;
#endif

    resp.key_count = ZMK_KEYMAP_LEN;
    resp.layer_count = ZMK_KEYMAP_LAYERS_LEN;

    struct zmk_physical_layout const *const *layouts;
    resp.layout_count = (uint32_t)zmk_physical_layouts_get_list(&layouts);

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW) && DT_HAS_CHOSEN(zmk_underglow)
    resp.led_count = DT_PROP(DT_CHOSEN(zmk_underglow), chain_length);
#endif

#if IS_ENABLED(CONFIG_HWINFO)
    uint32_t reset_cause = 0;

    if (hwinfo_get_reset_cause(&reset_cause) == 0) {
        resp.reset_cause_raw = reset_cause;
        copy_str(resp.reset_cause, sizeof(resp.reset_cause), reset_cause_text(reset_cause));
    }
    // Keep the reset cause for the 2.4G boot announcement.
#endif

    resp.has_rgb = IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW);
    resp.has_backlight = IS_ENABLED(CONFIG_ZMK_BACKLIGHT);
    resp.has_imu = IS_ENABLED(CONFIG_ZMK_MOTION);
    resp.has_display = IS_ENABLED(CONFIG_ZMK_DISPLAY);
    resp.has_split = IS_ENABLED(CONFIG_ZMK_SPLIT);
    resp.has_pointing = IS_ENABLED(CONFIG_ZMK_POINTING);
    resp.has_ble = IS_ENABLED(CONFIG_ZMK_BLE);
    resp.has_usb = IS_ENABLED(CONFIG_ZMK_USB);
    resp.has_2g4 = IS_ENABLED(CONFIG_ZMK_2G4);

    return SYSINFO_RESPONSE(hardware, resp);
}

static zmk_studio_Response get_runtime(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_sysinfo_RuntimeState resp = zmk_sysinfo_RuntimeState_init_zero;

    resp.uptime_s = (uint32_t)(k_uptime_get() / 1000);

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    resp.battery_percent = (int32_t)zmk_battery_state_of_charge();
#else
    // The protocol uses -1 when battery reporting is unavailable.
    resp.battery_percent = -1;
#endif

    // battery.c does not expose millivolts or charging state.
    resp.battery_mv = 0;
    resp.charging = false;

    zmk_keymap_layers_state_t layer_state = zmk_keymap_layer_state();
    resp.active_layer = 0;

    for (int i = 0; i < MIN(ZMK_KEYMAP_LAYERS_LEN, 32); i++) {
        if (layer_state & BIT(i)) {
            resp.active_layer = (uint32_t)i;
            break;
        }
    }

    resp.highest_layer = (uint32_t)zmk_keymap_highest_layer_active();

#if IS_ENABLED(CONFIG_ZMK_USB)
    resp.usb_present = zmk_usb_is_powered();
    resp.hid_ready = zmk_usb_is_hid_ready();
    resp.vddh_mv = resp.usb_present ? 5000 : 0;
#endif

#if IS_ENABLED(CONFIG_ZMK_2G4)
    resp.two_g4_paired = zmk_2g4_addr_is_paired() && zmk_2g4_crypto_has_paired_key();
#endif

    zmk_endpoint_instance_to_str(zmk_endpoints_selected(), resp.endpoint, sizeof(resp.endpoint));

#if IS_ENABLED(CONFIG_ZMK_BLE)
    int ble_profile = zmk_ble_active_profile_index();

    resp.ble_profile = ble_profile >= 0 ? (uint32_t)ble_profile : 0;
    resp.ble_connected = zmk_ble_active_profile_is_connected();
#endif

    resp.idle_state = (uint32_t)zmk_activity_get_state();

#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    bool rgb_on = false;

    if (zmk_rgb_underglow_get_state(&rgb_on) == 0) {
        resp.rgb_enabled = rgb_on;
    }
#endif

    return SYSINFO_RESPONSE(runtime, resp);
}

static zmk_studio_Response get_diagnostics(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_sysinfo_Diagnostics resp = zmk_sysinfo_Diagnostics_init_zero;

#if IS_ENABLED(CONFIG_INIT_STACKS) && IS_ENABLED(CONFIG_THREAD_STACK_INFO) &&                    \
    defined(CONFIG_ZMK_STUDIO_RPC_THREAD_STACK_SIZE)
    resp.rpc_stack_used_pct =
        stack_used_pct(k_current_get(), CONFIG_ZMK_STUDIO_RPC_THREAD_STACK_SIZE);
#endif

#if IS_ENABLED(CONFIG_INIT_STACKS) && IS_ENABLED(CONFIG_THREAD_STACK_INFO) &&                      \
    IS_ENABLED(CONFIG_SYSTEM_WORKQUEUE) && defined(CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE)
    resp.workq_stack_used_pct =
        stack_used_pct(&k_sys_work_q.thread, CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE);
#endif

    // Optional diagnostics not exposed by this build.
    resp.heap_free = 0;
    resp.settings_save_pending = 0;
    resp.two_g4_tx_total = 0;
    resp.two_g4_tx_fail = 0;
    resp.studio_rpc_dropped = 0;

    return SYSINFO_RESPONSE(diagnostics, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(sysinfo, get_identity, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(sysinfo, get_firmware, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(sysinfo, get_hardware, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(sysinfo, get_runtime, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(sysinfo, get_diagnostics, ZMK_STUDIO_RPC_HANDLER_UNSECURED);

