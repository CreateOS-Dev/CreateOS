// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "arch/apic.h"
#include "drivers/acpi.h"
#include "arch/x86/cpu.h"
#include "arch/x86/io.h"
#include <stdint.h>

#define APIC_MSR 0x1B
#define APIC_ENABLE (1U << 11)
#define APIC_SVR 0xF0
#define APIC_EOI 0xB0
#define APIC_SPURIOUS_VECTOR 0xFF
#define IOAPIC_REGSEL 0x00
#define IOAPIC_WINDOW 0x10
#define IOAPIC_REDIR_BASE 0x10
#define IOAPIC_MASK (1U << 16)
#define IOAPIC_POLARITY_LOW (1U << 13)
#define IOAPIC_TRIGGER_LEVEL (1U << 15)

static volatile uint32_t* local_apic;
static volatile uint32_t* ioapic;
static int enabled;
static uint32_t ioapic_gsi_base;
static uint32_t ioapic_redirection_count;

static uint32_t ioapic_read(uint8_t reg)
{
    ioapic[IOAPIC_REGSEL / 4] = reg;
    return ioapic[IOAPIC_WINDOW / 4];
}

static void ioapic_write(uint8_t reg, uint32_t value)
{
    ioapic[IOAPIC_REGSEL / 4] = reg;
    ioapic[IOAPIC_WINDOW / 4] = value;
}

static int ioapic_route_irq(uint8_t irq, uint8_t vector)
{
    uint32_t gsi = irq;
    uint32_t destination;
    uint16_t flags = 0;
    uint16_t polarity;
    uint16_t trigger;
    uint32_t pin;
    uint8_t redirection;
    uint32_t low = vector;
    int has_override = acpi_irq_override(irq, &gsi, &flags);
    if (has_override) {
        polarity = flags & 0x3U;
        trigger = (flags >> 2) & 0x3U;
        if (polarity == 2 || trigger == 2)
            return 0;
        if (polarity == 3)
            low |= IOAPIC_POLARITY_LOW;
        if (trigger == 3)
            low |= IOAPIC_TRIGGER_LEVEL;
    }
    if (gsi < ioapic_gsi_base)
        return 0;
    pin = gsi - ioapic_gsi_base;
    if (pin >= ioapic_redirection_count)
        return 0;
    redirection = (uint8_t)(IOAPIC_REDIR_BASE + pin * 2);
    destination = local_apic[0x20 / 4] >> 24;
    ioapic_write(redirection + 1, destination << 24);
    ioapic_write(redirection, low);
    return 1;
}

int apic_init(void)
{
    uint64_t base;
    uint32_t version;

    enabled = 0;
    if (!cpu_has_msr() || !cpu_has_apic() || !acpi_madt_available() ||
        !acpi_ioapic_count() || !acpi_local_apic_address()) return 0;
    base = cpu_read_msr(APIC_MSR);
    base = (base & ~0xFFFULL) | APIC_ENABLE;
    cpu_write_msr(APIC_MSR, base);
    local_apic = (volatile uint32_t*)(uintptr_t)acpi_local_apic_address();
    ioapic = (volatile uint32_t*)(uintptr_t)acpi_first_ioapic_address();
    version = ioapic_read(1);
    if ((version & 0xFF) == 0) return 0;
    ioapic_gsi_base = acpi_first_ioapic_gsi_base();
    ioapic_redirection_count = ((version >> 16) & 0xFFU) + 1U;
    local_apic[APIC_SVR / 4] = APIC_ENABLE | APIC_SPURIOUS_VECTOR;
    for (uint32_t pin = 0; pin < ioapic_redirection_count; pin++)
        ioapic_write((uint8_t)(IOAPIC_REDIR_BASE + pin * 2),
                     IOAPIC_MASK | 0x20);
    if (!ioapic_route_irq(0, 32) || !ioapic_route_irq(1, 33) ||
        !ioapic_route_irq(12, 44))
        return 0;
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    enabled = 1;
    return 1;
}

int apic_available(void) { return enabled; }

void apic_eoi(void)
{
    if (local_apic) local_apic[APIC_EOI / 4] = 0;
}