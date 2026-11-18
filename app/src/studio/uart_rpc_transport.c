/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

#include <zephyr/logging/log.h>
#include <zmk/studio/rpc.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

/* change this to any other UART peripheral if desired */
#define UART_DEVICE_NODE DT_CHOSEN(zmk_studio_rpc_uart)

static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);
static struct k_spinlock uart_state_lock;
static uint32_t uart_session;
#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
static uint8_t tx_pending[64];
static size_t tx_pending_len, tx_pending_offset;
#endif

static void tx_notify(uint32_t session) {
#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
    k_spinlock_key_t key = k_spin_lock(&uart_state_lock);
    if (uart_session == session) {
        uart_irq_tx_enable(uart_dev);
    }
    k_spin_unlock(&uart_state_lock, key);
#else
    uint8_t bytes[64];
    uint32_t len;
    while ((len = zmk_rpc_tx_read(ZMK_TRANSPORT_USB, session, bytes, sizeof(bytes)))) {
        for (uint32_t i = 0; i < len; i++) {
            uart_poll_out(uart_dev, bytes[i]);
        }
    }
#endif
}

#if !IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
static void uart_rx_main(void) {
    for (;;) {
        uint32_t session = zmk_rpc_transport_session(ZMK_TRANSPORT_USB);
        uint8_t byte;
        if (session && uart_poll_in(uart_dev, &byte) == 0) {
            zmk_rpc_rx_write(ZMK_TRANSPORT_USB, session, &byte, 1);
        } else {
            k_sleep(K_MSEC(session ? 1 : 10));
        }
    }
}

K_THREAD_DEFINE(uart_transport_read_thread, CONFIG_ZMK_STUDIO_TRANSPORT_UART_RX_STACK_SIZE,
                uart_rx_main, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
#endif

static int start_rx(void) {
    k_spinlock_key_t key = k_spin_lock(&uart_state_lock);
    uart_session = zmk_rpc_transport_session(ZMK_TRANSPORT_USB);
#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
    tx_pending_len = tx_pending_offset = 0;
    uart_irq_rx_enable(uart_dev);
#endif
    k_spin_unlock(&uart_state_lock, key);
    return 0;
}

static int stop_rx(void) {
    k_spinlock_key_t key = k_spin_lock(&uart_state_lock);
    uart_session = 0;
#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
    uart_irq_rx_disable(uart_dev);
    uart_irq_tx_disable(uart_dev);
    tx_pending_len = tx_pending_offset = 0;
#endif
    k_spin_unlock(&uart_state_lock, key);
    return 0;
}

ZMK_RPC_TRANSPORT(uart, ZMK_TRANSPORT_USB, start_rx, stop_rx, tx_notify);

#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)

/*
 * Read characters from UART until line end is detected. Afterwards push the
 * data to the message queue.
 */
static void serial_cb(const struct device *dev, void *user_data) {
    if (!uart_irq_update(uart_dev)) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&uart_state_lock);
    uint32_t session = uart_session;
    if (!session || session != zmk_rpc_transport_session(ZMK_TRANSPORT_USB)) {
        uart_irq_rx_disable(uart_dev);
        uart_irq_tx_disable(uart_dev);
        k_spin_unlock(&uart_state_lock, key);
        return;
    }
    if (uart_irq_rx_ready(uart_dev)) {
        uint8_t bytes[64];
        int len;
        do {
            len = uart_fifo_read(uart_dev, bytes, sizeof(bytes));
            if (len > 0 && zmk_rpc_rx_write(ZMK_TRANSPORT_USB, session, bytes, len) != (uint32_t)len) {
                LOG_WRN("Dropping RPC input: session changed or RX buffer full");
            }
        } while (len == (int)sizeof(bytes));
    }
    if (uart_irq_tx_ready(uart_dev)) {
        if (tx_pending_offset == tx_pending_len) {
            tx_pending_offset = 0;
            tx_pending_len = zmk_rpc_tx_read(ZMK_TRANSPORT_USB, session,
                                             tx_pending, sizeof(tx_pending));
        }
        if (!tx_pending_len) {
            uart_irq_tx_disable(uart_dev);
        } else {
            int sent = uart_fifo_fill(uart_dev, tx_pending + tx_pending_offset,
                                     tx_pending_len - tx_pending_offset);
            if (sent > 0) {
                tx_pending_offset += MIN((size_t)sent, tx_pending_len - tx_pending_offset);
            }
        }
    }
    k_spin_unlock(&uart_state_lock, key);
}

#endif

static int uart_rpc_interface_init(void) {
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not found!");
        return -ENODEV;
    }

#if IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)
    /* configure interrupt and callback to receive data */
    int ret = uart_irq_callback_user_data_set(uart_dev, serial_cb, NULL);

    if (ret < 0) {
        if (ret == -ENOTSUP) {
            printk("Interrupt-driven UART API support not enabled\n");
        } else if (ret == -ENOSYS) {
            printk("UART device does not support interrupt-driven API\n");
        } else {
            printk("Error setting UART callback: %d\n", ret);
        }
        return ret;
    }
#endif // IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN)

    return 0;
}

SYS_INIT(uart_rpc_interface_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
