#ifndef VIBEOS_TIMER_H
#define VIBEOS_TIMER_H

#include "types.h"

#define TIMER_HZ 100u

void timer_init(void);
void timer_tick(void);
void timer_acknowledge_irq(void);
uint64_t timer_tick_count(void);
uint32_t timer_frequency_hz(void);

#endif
