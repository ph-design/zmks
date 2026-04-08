#pragma once

#include <zephyr/irq.h>
#include <nrfx.h>

static inline void zmk_ble_hardware_control(bool enable) {
    if (!enable) {
        irq_disable(TIMER0_IRQn);
        irq_disable(RTC0_IRQn);
        NRF_PPI->CHENCLR = 0xFFFFFFFF;
        NRF_TIMER0->TASKS_STOP = 1;
        NRF_TIMER0->TASKS_CLEAR = 1;
        NRF_TIMER0->EVENTS_COMPARE[0] = 0;
        NRF_TIMER0->EVENTS_COMPARE[1] = 0;
        NRF_RTC0->TASKS_STOP = 1;
        NRF_RTC0->TASKS_CLEAR = 1;
        NRF_RTC0->EVENTS_COMPARE[0] = 0;
        NRF_CCM->ENABLE = 0;
        NRF_AAR->ENABLE = 0;
    } else {
        irq_enable(TIMER0_IRQn);
        irq_enable(RTC0_IRQn);
    }
}
