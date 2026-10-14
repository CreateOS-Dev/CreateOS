#include "drivers/hpet.h"
#include "memory/vmm.h"
#include <stdint.h>

#define HPET_REGISTER_CAPABILITIES 0x000
#define HPET_REGISTER_CONFIGURATION 0x010
#define HPET_REGISTER_MAIN_COUNTER 0x0F0
#define HPET_CONFIGURATION_ENABLE (1ULL << 0)
#define HPET_CONFIGURATION_LEGACY_REPLACEMENT (1ULL << 1)
#define HPET_CAPABILITIES_PERIOD_SHIFT 32
#define HPET_CAPABILITIES_PERIOD_MASK 0xFFFFFFFFULL
#define HPET_CAPABILITIES_COUNTER_64BIT (1ULL << 13)
#define HPET_MAX_PERIOD_FEMTOSECONDS 100000000ULL
#define HPET_FEMTOSECONDS_PER_NANOSECOND 1000000ULL

static volatile uint8_t* registers;
static uint64_t period_femtoseconds;
static uint64_t counter_origin;
static int initialized;

static uint64_t hpet_read_register(uint32_t offset)
{
    return *(volatile uint64_t*)(registers + offset);
}

static void hpet_write_register(uint32_t offset, uint64_t value)
{
    *(volatile uint64_t*)(registers + offset) = value;
}

int hpet_init(uint64_t physical_base)
{
    uint64_t capabilities;
    uint64_t configuration;
    uint64_t first_counter;
    uint64_t second_counter;
    uint64_t period;

    initialized = 0;
    registers = 0;
    period_femtoseconds = 0;
    counter_origin = 0;

    if (physical_base < 0x1000 || physical_base > 0xFFFFFEFFULL ||
        (physical_base & 7U))
        return 0;

    registers = (volatile uint8_t*)vmm_map_mmio_range(physical_base, 0x100);
    if (!registers)
        return 0;
    capabilities = hpet_read_register(HPET_REGISTER_CAPABILITIES);
    period = (capabilities >> HPET_CAPABILITIES_PERIOD_SHIFT) &
             HPET_CAPABILITIES_PERIOD_MASK;
    if (!(capabilities & HPET_CAPABILITIES_COUNTER_64BIT) ||
        period == 0 || period > HPET_MAX_PERIOD_FEMTOSECONDS) {
        registers = 0;
        return 0;
    }

    period_femtoseconds = period;
    configuration = hpet_read_register(HPET_REGISTER_CONFIGURATION);
    configuration &= ~HPET_CONFIGURATION_LEGACY_REPLACEMENT;
    configuration |= HPET_CONFIGURATION_ENABLE;
    hpet_write_register(HPET_REGISTER_CONFIGURATION, configuration);

    first_counter = hpet_read_register(HPET_REGISTER_MAIN_COUNTER);
    for (uint32_t attempt = 0; attempt < 100000; attempt++) {
        second_counter = hpet_read_register(HPET_REGISTER_MAIN_COUNTER);
        if (second_counter != first_counter) {
            counter_origin = first_counter;
            initialized = 1;
            return 1;
        }
        __asm__ volatile("pause");
    }

    hpet_write_register(HPET_REGISTER_CONFIGURATION,
                        configuration & ~HPET_CONFIGURATION_ENABLE);
    registers = 0;
    period_femtoseconds = 0;
    return 0;
}

int hpet_available(void)
{
    return initialized;
}

int hpet_monotonic_ns(uint64_t* nanoseconds)
{
    uint64_t elapsed_ticks;
    uint64_t whole;
    uint64_t remainder;
    uint64_t current;
    uint64_t elapsed_nanoseconds;
    uint64_t fractional_nanoseconds;

    if (!initialized || !nanoseconds)
        return 0;

    current = hpet_read_register(HPET_REGISTER_MAIN_COUNTER);
    if (current < counter_origin)
        return 0;
    elapsed_ticks = current - counter_origin;
    whole = elapsed_ticks / HPET_FEMTOSECONDS_PER_NANOSECOND;
    remainder = elapsed_ticks % HPET_FEMTOSECONDS_PER_NANOSECOND;
    if (whole > UINT64_MAX / period_femtoseconds)
        return 0;
    elapsed_nanoseconds = whole * period_femtoseconds;
    fractional_nanoseconds = remainder * period_femtoseconds /
                             HPET_FEMTOSECONDS_PER_NANOSECOND;
    if (fractional_nanoseconds > UINT64_MAX - elapsed_nanoseconds)
        return 0;
    *nanoseconds = elapsed_nanoseconds + fractional_nanoseconds;
    return 1;
}
