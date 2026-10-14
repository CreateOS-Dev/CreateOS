// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "arch/x86_64/interrupts.h"
#include "arch/apic.h"
#include "arch/x86_64/io.h"
#include "drivers/keyboard.h"
#include "drivers/acpi.h"
#include "drivers/serial.h"
#include "drivers/timer.h"
#include "drivers/framebuffer.h"
#include "drivers/vga.h"
#include "proc/appl.h"
#include "proc/user.h"
#include <stddef.h>
#include <stdint.h>

#define IDT_ENTRIES 256
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20
#define ICW1_INIT    0x10
#define ICW1_ICW4    0x01
#define ICW4_8086    0x01

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
extern void* interrupt_stub_table[48];
extern void idt_load(const struct idt_pointer* pointer);

static void serial_put_uint(unsigned long value)
{
    char buffer[24];
    int index = 23;
    buffer[index] = '\0';
    do {
        buffer[--index] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && index > 0);
    serial_puts(&buffer[index]);
}

static void serial_put_hex(unsigned long value)
{
    char buffer[19];
    static const char digits[] = "0123456789abcdef";
    buffer[0] = '0';
    buffer[1] = 'x';
    for (int i = 0; i < 16; i++)
        buffer[i + 2] = digits[(value >> (60 - i * 4)) & 0x0F];
    buffer[18] = '\0';
    serial_puts(buffer);
}

static void panic_write(const char* text)
{
    if (framebuffer_ready()) framebuffer_write(text, 0x0F);
    vga_print(text, 0x0F);
}

static void panic_write_uint(unsigned long value)
{
    char buffer[24];
    int index = 23;
    buffer[index] = 0;
    do {
        buffer[--index] = (char)('0' + (value % 10));
        value /= 10;
    } while (value && index > 0);
    panic_write(&buffer[index]);
}

static void panic_write_hex(unsigned long value)
{
    char buffer[19];
    int index = 18;
    buffer[index] = 0;
    do {
        unsigned int digit = (unsigned int)(value & 0x0F);
        buffer[--index] = (char)(digit < 10 ? '0' + digit : 'A' + digit - 10);
        value >>= 4;
    } while (value && index > 2);
    buffer[--index] = 'x';
    buffer[--index] = '0';
    panic_write(&buffer[index]);
}

static void idt_set_gate(uint8_t vector, void* handler)
{
    uint64_t address = (uint64_t)handler;
    idt[vector].offset_low = (uint16_t)address;
    idt[vector].selector = 0x08;
    idt[vector].ist = 1;
    idt[vector].type_attr = 0x8E;
    idt[vector].offset_mid = (uint16_t)(address >> 16);
    idt[vector].offset_high = (uint32_t)(address >> 32);
    idt[vector].zero = 0;
}

static void pic_remap(void)
{
    uint8_t master_mask = inb(PIC1_DATA);
    uint8_t slave_mask = inb(PIC2_DATA);
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC1_DATA, 32);
    outb(PIC2_DATA, 40);
    outb(PIC1_DATA, 4);
    outb(PIC2_DATA, 2);
    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);
    outb(PIC1_DATA, (uint8_t)(master_mask & 0xFC));
    outb(PIC2_DATA, slave_mask);
}

void interrupts_init(void)
{
    struct idt_pointer pointer;
    for (uint16_t i = 0; i < IDT_ENTRIES; i++) {
        idt[i].offset_low = 0;
        idt[i].selector = 0;
        idt[i].ist = 0;
        idt[i].type_attr = 0;
        idt[i].offset_mid = 0;
        idt[i].offset_high = 0;
        idt[i].zero = 0;
    }
    for (uint8_t i = 0; i < 48; i++) idt_set_gate(i, interrupt_stub_table[i]);
    pic_remap();
    pointer.limit = sizeof(idt) - 1;
    pointer.base = (uint64_t)idt;
    idt_load(&pointer);
    __asm__ volatile("sti" ::: "memory");
}

static void panic_wait_for_key(void)
{
    for (;;) {
        uint8_t status = inb(0x64);
        if (!(status & 0x01) || (status & 0x20))
            continue;
        if (!(inb(0x60) & 0x80))
            acpi_reboot();
    }
}

static void panic_screen(unsigned long vector, unsigned long error,
                         unsigned long rip, unsigned long cs,
                         unsigned long rflags)
{
    unsigned long fault_address = 0;
    if (vector == 14)
        __asm__ volatile("mov %%cr2, %0" : "=r"(fault_address));
    __asm__ volatile("cli");
    if (framebuffer_ready()) {
        framebuffer_set_font(0);
        framebuffer_set_scale(2);
        framebuffer_fill(0x000000);
        panic_write("  +--------------------------------------------------------------+\n");
        panic_write("  | CreateOS 0.0.1                         SYSTEM HALTED       |\n");
        panic_write("  +--------------------------------------------------------------+\n\n");
        panic_write("  A fatal CPU exception stopped CreateOS.\n");
        panic_write("  Reboot the computer. Unsaved information may be lost.\n\n");
        panic_write("  Error details\n");
        panic_write("  -------------\n");
        panic_write("  Exception vector: ");
        panic_write_uint(vector);
        panic_write("\n  Error code: ");
        panic_write_uint(error);
        panic_write("\n  Instruction pointer: ");
        panic_write_hex(rip);
        panic_write("\n  Code segment: ");
        panic_write_hex(cs);
        panic_write("\n  Flags: ");
        panic_write_hex(rflags);
        if (vector == 14) {
            panic_write("\n  Fault address: ");
            panic_write_hex(fault_address);
        }
        panic_write("\n\n  System halted. Press any key to restart CreateOS.\n");
    }
    panic_write("\nKERNEL PANIC: CPU EXCEPTION\nVector: ");
    panic_write_uint(vector);
    panic_write(" Error: ");
    panic_write_uint(error);
    panic_write(" RIP: 0x");
    panic_write_hex(rip);
    panic_write(" CS: 0x");
    panic_write_hex(cs);
    panic_write(" RFLAGS: 0x");
    panic_write_hex(rflags);
    panic_write("\nSystem halted.\n");
    serial_set_display_mirror(0);
    serial_puts("KERNEL PANIC: exception vector=");
    serial_put_uint(vector);
    serial_puts(" error=");
    serial_put_uint(error);
    serial_puts(" rip=");
    serial_put_hex(rip);
    serial_puts(" cs=");
    serial_put_hex(cs);
    serial_puts(" rflags=");
    serial_put_hex(rflags);
    if (vector == 14) {
        serial_puts(" cr2=");
        serial_put_hex(fault_address);
    }
    serial_puts("\n");
    panic_wait_for_key();
}

uint64_t interrupt_dispatch(unsigned long vector, unsigned long error_code,
                            unsigned long rip, unsigned long cs,
                            unsigned long rflags)
{
    if (vector < 32) {
        if ((cs & 3) &&
            (vector == 0 || vector == 6 || vector == 12 ||
             vector == 13 || vector == 14 || vector == 17)) {
            unsigned long fault_address = 0;
            const char* recovery_program =
                kernel_user_current() ? "/bin/sh" : "/bin/login";
            if (vector == 14)
                __asm__ volatile("mov %%cr2, %0" : "=r"(fault_address));
            serial_puts("[user fault] vector=");
            serial_put_uint(vector);
            serial_puts(" error=");
            serial_put_uint(error_code);
            serial_puts(" rip=");
            serial_put_hex(rip);
            if (vector == 14) {
                serial_puts(" cr2=");
                serial_put_hex(fault_address);
            }
            serial_puts("; restarting ");
            serial_puts(recovery_program);
            serial_puts("\n");

            if ((uint32_t)user_exec_from_user(recovery_program, NULL) ==
                0xDEADBEEF)
                return 0xDEADBEEF;
            serial_puts("[user fault] unable to start recovery program\n");
        }
        panic_screen(vector, error_code, rip, cs, rflags);
    }
    if (vector == 32) timer_tick();
    if (vector == 33) keyboard_irq_handler();
    if (apic_available() && vector >= 32 && vector < 256) {
        apic_eoi();
    } else if (vector >= 32 && vector < 40) outb(PIC1_COMMAND, PIC_EOI);
    if (!apic_available() && vector >= 40 && vector < 48) {
        outb(PIC2_COMMAND, PIC_EOI);
        outb(PIC1_COMMAND, PIC_EOI);
    }
    return 0;
}
