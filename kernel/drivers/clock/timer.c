// ============================================================
// CreateOS 0.0.1
// ============================================================


#include "drivers/timer.h"
#include "drivers/framebuffer.h"
#include "drivers/vga.h"
#include "arch/x86_64/io.h"

volatile uint64_t g_ticks = 0;

void timer_tick(void) {
    g_ticks++;
    framebuffer_pointer_tick(g_ticks);
}

void timer_init(void) {
    uint32_t divisor = 1193180 / TIMER_HZ;

    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)(divisor >> 8));
}