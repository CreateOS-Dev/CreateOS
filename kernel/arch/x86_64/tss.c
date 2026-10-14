// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "arch/x86_64/tss.h"
#include "drivers/serial.h"

extern uint64_t irq_stack_top;

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct tss64 tss64 __attribute__((aligned(16)));

void tss_init(uint64_t ring0_stack)
{
    tss64.rsp0 = ring0_stack;
    tss64.ist[0] = (uint64_t)(uintptr_t)&irq_stack_top;
    tss64.iomap_base = sizeof(tss64);
}

void tss_load(void)
{
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr;
    uint8_t* descriptor;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    if (gdtr.limit < 0x37) {
        serial_puts("TSS : ERROR - GDT does not contain the kernel TSS descriptor\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    descriptor = (uint8_t*)(uintptr_t)(gdtr.base + 0x28);
    if (!(descriptor[5] & 0x80) ||
        ((descriptor[5] & 0x0F) != 0x09 &&
         (descriptor[5] & 0x0F) != 0x0B)) {
        serial_puts("TSS : ERROR - kernel TSS descriptor is invalid\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    descriptor[5] = (uint8_t)((descriptor[5] & 0xF0) | 0x09);
    __asm__ volatile ("ltr %%ax" : : "a"((uint16_t)0x28));
}
