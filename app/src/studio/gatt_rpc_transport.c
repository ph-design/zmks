/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <sys/types.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/ring_buffer.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/studio/rpc.h>

#include "uuid.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

static atomic_t rx_session, notify_session;

// Keep ATT waits and retries off the system workqueue.
K_THREAD_STACK_DEFINE(gatt_tx_stack, CONFIG_ZMK_STUDIO_TRANSPORT_BLE_TX_STACK_SIZE);
static struct k_work_q gatt_tx_work_q;

static void rpc_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);

    bool notif_enabled = (value == BT_GATT_CCC_INDICATE);

    LOG_INF("RPC Notifications %s", notif_enabled ? "enabled" : "disabled");

#if CONFIG_ZMK_STUDIO_TRANSPORT_BLE_PREF_LATENCY < CONFIG_BT_PERIPHERAL_PREF_LATENCY
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    if (conn) {
        uint8_t latency = notif_enabled ? CONFIG_ZMK_STUDIO_TRANSPORT_BLE_PREF_LATENCY
                                        : CONFIG_BT_PERIPHERAL_PREF_LATENCY;

        int ret = bt_conn_le_param_update(
            conn,
            BT_LE_CONN_PARAM(CONFIG_BT_PERIPHERAL_PREF_MIN_INT, CONFIG_BT_PERIPHERAL_PREF_MAX_INT,
                             latency, CONFIG_BT_PERIPHERAL_PREF_TIMEOUT));
        if (ret < 0) {
            LOG_WRN("Failed to request lower latency while studio is active (%d)", ret);
        }

        bt_conn_unref(conn);
    }
#endif
}

static ssize_t read_rpc_resp(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                             uint16_t len, uint16_t offset) {

    LOG_DBG("Read response for length %d at offset %d", len, offset);
    return 0;
}

static ssize_t write_rpc_req(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                             uint16_t len, uint16_t offset, uint8_t flags) {
    uint32_t session = (uint32_t)atomic_get(&rx_session);
    struct bt_conn *active = zmk_ble_active_profile_conn();
    bool selected = active && active == conn;
    if (active) {
        bt_conn_unref(active);
    }
    if (!selected || !session) {
        return BT_GATT_ERR(BT_ATT_ERR_WRITE_NOT_PERMITTED);
    }
    if (zmk_rpc_rx_write(ZMK_TRANSPORT_BLE, session, buf, len) != len) {
        return BT_GATT_ERR(BT_ATT_ERR_INSUFFICIENT_RESOURCES);
    }

    return len;
}

BT_GATT_SERVICE_DEFINE(
    rpc_interface, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(ZMK_STUDIO_BT_SERVICE_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_STUDIO_BT_RPC_CHRC_UUID),
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_READ | BT_GATT_CHRC_INDICATE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT, read_rpc_resp,
                           write_rpc_req, NULL),
    BT_GATT_CCC(rpc_ccc_cfg_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

static uint16_t get_notify_size_for_conn(struct bt_conn *conn) {
    return bt_gatt_get_mtu(conn) - 3;
}

static int gatt_start_rx() {
    atomic_set(&rx_session, zmk_rpc_transport_session(ZMK_TRANSPORT_BLE));
    return 0;
}

static int gatt_stop_rx(void) {
    atomic_clear(&rx_session);
    atomic_clear(&notify_session);
    return 0;
}

static K_SEM_DEFINE(indication_done, 0, 1);
static uint8_t indication_error;

static void indication_confirmed(struct bt_conn *conn, struct bt_gatt_indicate_params *params,
                                 uint8_t err) {
    ARG_UNUSED(conn);
    ARG_UNUSED(params);
    indication_error = err;
}

static void indication_released(struct bt_gatt_indicate_params *params) {
    ARG_UNUSED(params);
    k_sem_give(&indication_done);
}

static struct bt_gatt_indicate_params rpc_indicate_params = {
    .attr = &rpc_interface.attrs[1],
    .func = indication_confirmed,
    .destroy = indication_released,
};

static void notif_rpc_tx_cb(struct k_work *work) {
    uint32_t session = (uint32_t)atomic_get(&notify_session);
    if (!session || session != zmk_rpc_transport_session(ZMK_TRANSPORT_BLE)) {
        return;
    }
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    if (!conn) {
        LOG_WRN("No active connection for queued data");
        return;
    }
    uint16_t notify_size = get_notify_size_for_conn(conn);
    uint8_t notify_bytes[notify_size];

    while (session == zmk_rpc_transport_session(ZMK_TRANSPORT_BLE)) {
        uint16_t added = zmk_rpc_tx_read(ZMK_TRANSPORT_BLE, session,
                                        notify_bytes, sizeof(notify_bytes));
        if (!added) {
            break;
        }
        rpc_indicate_params.data = notify_bytes;
        rpc_indicate_params.len = added;

        if (session != zmk_rpc_transport_session(ZMK_TRANSPORT_BLE)) {
            break;
        }
        int err = bt_gatt_indicate(conn, &rpc_indicate_params);
        if (!err) {
            k_sem_take(&indication_done, K_FOREVER);
            err = indication_error ? -EIO : 0;
        }
        if (err) {
            LOG_WRN("Studio BLE indication failed: %d", err);
            zmk_rpc_transport_fail(ZMK_TRANSPORT_BLE, session);
            break;
        }
    }
    bt_conn_unref(conn);
}

static K_WORK_DEFINE(notify_tx_work, notif_rpc_tx_cb);

static void gatt_tx_notify(uint32_t session) {
    atomic_set(&notify_session, session);
    k_work_submit_to_queue(&gatt_tx_work_q, &notify_tx_work);
}

ZMK_RPC_TRANSPORT(gatt, ZMK_TRANSPORT_BLE, gatt_start_rx, gatt_stop_rx, gatt_tx_notify);

static int gatt_rpc_listener(const zmk_event_t *eh) {
    uint32_t session = zmk_rpc_transport_session(ZMK_TRANSPORT_BLE);
    if (session) {
        uint32_t next = zmk_rpc_transport_reset(ZMK_TRANSPORT_BLE, session);
        if (next) {
            atomic_set(&rx_session, next);
            atomic_set(&notify_session, next);
        }
    }

#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOCK_ON_DISCONNECT)
    struct bt_conn *conn = zmk_ble_active_profile_conn();

    if (!conn) {
        zmk_studio_core_lock();
    } else {
        bt_conn_unref(conn);
    }
#endif

    return 0;
}

ZMK_LISTENER(gatt_rpc_listener, gatt_rpc_listener);
ZMK_SUBSCRIPTION(gatt_rpc_listener, zmk_ble_active_profile_changed);

static int gatt_rpc_init(void) {
    static const struct k_work_queue_config queue_config = {.name = "Studio BLE Send Work"};
    k_work_queue_start(&gatt_tx_work_q, gatt_tx_stack, K_THREAD_STACK_SIZEOF(gatt_tx_stack),
                       CONFIG_ZMK_STUDIO_TRANSPORT_BLE_TX_PRIORITY, &queue_config);
    return 0;
}

SYS_INIT(gatt_rpc_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
