/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Marvin Kicha <https://github.com/MarvInt64/> */

#include "timer.h"
#include "io.h"

#define PIC1_COMMAND 0x20u
#define PIC1_DATA 0x21u
#define PIC2_COMMAND 0xa0u
#define PIC2_DATA 0xa1u
#define PIT_COMMAND 0x43u
#define PIT_CHANNEL0 0x40u
#define TIMER_VECTOR 0x20u
#define PIT_DIVISOR ((uint16_t)(1193182u / TIMER_HZ))

static uint64_t g_timer_ticks;

static void pic_remap(void) {
    outb(PIC1_COMMAND, 0x11u);
    io_wait();
    outb(PIC2_COMMAND, 0x11u);
    io_wait();

    outb(PIC1_DATA, TIMER_VECTOR);
    io_wait();
    outb(PIC2_DATA, 0x28u);
    io_wait();

    outb(PIC1_DATA, 0x04u);
    io_wait();
    outb(PIC2_DATA, 0x02u);
    io_wait();

    outb(PIC1_DATA, 0x01u);
    io_wait();
    outb(PIC2_DATA, 0x01u);
    io_wait();

    outb(PIC1_DATA, 0xfeu);
    outb(PIC2_DATA, 0xffu);
}

static void pit_init(void) {
    outb(PIT_COMMAND, 0x36u);
    io_wait();
    outb(PIT_CHANNEL0, (uint8_t)(PIT_DIVISOR & 0xffu));
    io_wait();
    outb(PIT_CHANNEL0, (uint8_t)((PIT_DIVISOR >> 8) & 0xffu));
}

void timer_init(void) {
    g_timer_ticks = 0;
    pic_remap();
    pit_init();
}

void timer_tick(void) {
    ++g_timer_ticks;
}

void timer_acknowledge_irq(void) {
    outb(PIC1_COMMAND, 0x20u);
}

uint64_t timer_tick_count(void) {
    return g_timer_ticks;
}

uint32_t timer_frequency_hz(void) {
    return TIMER_HZ;
}
