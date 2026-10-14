// ============================================================
// CreateOS 0.0.1
// ============================================================

#include <stdint.h>
#include "arch/x86_64/random.h"

uintptr_t __stack_chk_guard;

void __stack_chk_fail(void) {

    __asm__ volatile ("cli");

    volatile char* vga = (char*)0xB8000;
    const char* msg = "STACK PROTECTION FAILURE: HALTING SYSTEM";
    int i = 0;
    while (msg[i] != '\0') {
        vga[i*2] = msg[i];
        vga[i*2+1] = 0x0C; // Light red on black
        i++;
    }

    for (;;) {
        __asm__ volatile ("hlt");
    }
}

__attribute__((no_stack_protector))
int __stack_chk_init(void)
{
    uintptr_t guard = 0;

    if (!x86_64_random_bytes(&guard, sizeof(guard)))
        return 0;
    guard &= ~(uintptr_t)0xFF;
    if (!guard)
        return 0;
    __stack_chk_guard = guard;
    return 1;
}