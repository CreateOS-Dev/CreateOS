// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#define TIMER_HZ 100

extern volatile uint64_t g_ticks;

void timer_init(void);
void timer_tick(void);

#endif
