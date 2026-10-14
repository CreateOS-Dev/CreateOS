// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "drivers/acpi.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/tss.h"
#include "drivers/timer.h"
#include "drivers/serial.h"
#include "fs/fs.h"
#include "syscall.h"
#include "lib/string.h"
#include <stdint.h>
#include <stddef.h>

#define RSDP_SIGNATURE 0x2052545020445352ULL
#define ACPI_MAX_ISA_IRQS 16
#define S3_WAKE_PHYSICAL 0x8000U
#define ACPI_PM1_SLEEP_TYPE_MASK (7U << 10)
#define ACPI_PM1_SLEEP_ENABLE (1U << 13)
#define ACPI_PM1_WAKE_STATUS (1U << 15)
#define ACPI_PM1_POWER_BUTTON_ENABLE (1U << 8)

typedef struct {
    uint32_t gsi;
    uint16_t flags;
    uint8_t valid;
} acpi_irq_override_t;

static int found;
static uint8_t revision;
static uint64_t address;
static uint64_t madt_address;
static uint32_t madt_entries;
static uint32_t local_apic_address;
static uint32_t ioapic_count;
static uint32_t first_ioapic_address;
static uint32_t first_ioapic_gsi_base;
static acpi_irq_override_t irq_overrides[ACPI_MAX_ISA_IRQS];
static uint64_t mcfg_base;
static uint8_t mcfg_start_bus;
static uint8_t mcfg_end_bus;
static uint64_t hpet_address;
static uint16_t reset_port;
static uint8_t reset_value;
static int reset_available;
static uint64_t facs_address;
static uint64_t dsdt_address;
static uint16_t pm1a_control_port;
static uint16_t pm1b_control_port;
static uint16_t pm1a_event_port;
static uint8_t pm1_event_length;
static uint8_t pm1_sleep_type_a;
static uint8_t pm1_sleep_type_b;
static int s3_supported;
static uint64_t idle_suspend_ticks;
static uint64_t last_activity_tick;
static int idle_suspend_active;

extern void acpi_arch_s3_enter(uint64_t pm1a, uint64_t pm1b,
                               uint64_t value_a, uint64_t value_b);
extern uint8_t s3_wake_template_start[];
extern uint8_t s3_wake_template_end[];
extern uint8_t s3_wake_64_entry[];
extern uint8_t s3_wake_data_rsp[];
extern uint8_t s3_wake_data_rip[];
extern uint8_t s3_wake_data_cr3[];
extern uint8_t s3_wake_data_cr4[];
extern uint64_t stack_top;

typedef struct {
    uint64_t signature;
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed)) rsdp_t;

typedef struct {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) sdt_header_t;

static int checksum_ok(const uint8_t* data, uint32_t length)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++) sum = (uint8_t)(sum + data[i]);
    return sum == 0;
}

static int signature_is(const char* value, const char* expected)
{
    for (int i = 0; i < 4; i++)
        if (value[i] != expected[i]) return 0;
    return 1;
}

static uint32_t read_u32(const uint8_t* value)
{
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
}

static uint64_t read_u64(const uint8_t* value)
{
    return (uint64_t)read_u32(value) |
           ((uint64_t)read_u32(value + 4) << 32);
}

static int aml_integer(const uint8_t* data, uint32_t end, uint32_t* offset,
                       uint8_t* value)
{
    uint64_t parsed;
    uint8_t prefix;
    uint32_t length;

    if (*offset >= end) return 0;
    prefix = data[(*offset)++];
    switch (prefix) {
    case 0x00: *value = 0; return 1;
    case 0x01: *value = 1; return 1;
    case 0xFF: *value = 7; return 1;
    case 0x0A: length = 1; break;
    case 0x0B: length = 2; break;
    case 0x0C: length = 4; break;
    case 0x0E: length = 8; break;
    default: return 0;
    }
    if (length > end - *offset) return 0;
    parsed = 0;
    for (uint32_t i = 0; i < length; i++)
        parsed |= (uint64_t)data[(*offset)++] << (i * 8);
    if (parsed > 7) return 0;
    *value = (uint8_t)parsed;
    return 1;
}

static int aml_s3_sleep_types(uint64_t address_value, uint8_t* type_a,
                              uint8_t* type_b)
{
    const uint8_t* table = (const uint8_t*)(uintptr_t)address_value;
    uint32_t length;
    uint32_t end;

    if (!address_value || address_value > 0xFFFFFFFFULL ||
        !signature_is((const char*)table, "DSDT"))
        return 0;
    length = read_u32(table + 4);
    if (length < sizeof(sdt_header_t) || length > 0x100000U ||
        !checksum_ok(table, length))
        return 0;
    end = length;
    for (uint32_t i = sizeof(sdt_header_t); i + 7 < end; i++) {
        if (table[i - 1] != 0x08 ||
            memcmp(table + i, "_S3_", 4) != 0)
            continue;
        uint32_t cursor = i + 4;
        if (cursor >= end || table[cursor++] != 0x12) continue;
        if (cursor >= end) continue;
        uint32_t package_start = cursor;
        uint8_t lead = table[cursor++];
        uint32_t follow = lead >> 6;
        uint32_t package_length = lead & (follow ? 0x0F : 0x3F);
        if (follow > 3 || follow > end - cursor) continue;
        for (uint32_t n = 0; n < follow; n++)
            package_length |= (uint32_t)table[cursor++] << (4 + 8 * n);
        if (package_length < cursor - package_start ||
            package_length > end - package_start)
            continue;
        uint32_t package_end = package_start + package_length;
        if (cursor >= package_end || table[cursor++] < 2) continue;
        if (!aml_integer(table, package_end, &cursor, type_a) ||
            !aml_integer(table, package_end, &cursor, type_b))
            continue;
        return 1;
    }
    return 0;
}

static int fadt_io_register(const uint8_t* fadt, uint32_t length,
                            uint32_t legacy_offset, uint32_t extended_offset,
                            uint16_t* port)
{
    if (length >= extended_offset + 12) {
        const uint8_t* gas = fadt + extended_offset;
        uint8_t address_space = gas[0];
        uint8_t bit_width = gas[1];
        uint64_t address_value = read_u64(gas + 4);
        if (address_space == 1 && bit_width >= 16 &&
            address_value > 0 && address_value <= 0xFFFF) {
            *port = (uint16_t)address_value;
            return 1;
        }
    }
    if (length >= legacy_offset + 4) {
        uint32_t address_value = read_u32(fadt + legacy_offset);
        if (address_value > 0 && address_value <= 0xFFFF) {
            *port = (uint16_t)address_value;
            return 1;
        }
    }
    return 0;
}

static void inspect_fadt(const uint8_t* fadt, uint32_t length)
{
    uint32_t facs32, dsdt32;
    uint64_t facs64 = 0, dsdt64 = 0;
    uint8_t control_length;

    if (length < 90) return;
    facs32 = read_u32(fadt + 36);
    dsdt32 = read_u32(fadt + 40);
    if (length >= 148) {
        facs64 = read_u64(fadt + 132);
        dsdt64 = read_u64(fadt + 140);
    }
    facs_address = facs64 ? facs64 : facs32;
    dsdt_address = dsdt64 ? dsdt64 : dsdt32;
    control_length = fadt[89];
    pm1_event_length = fadt[88];
    if (control_length < 2 ||
        !fadt_io_register(fadt, length, 64, 172, &pm1a_control_port))
        return;
    (void)fadt_io_register(fadt, length, 68, 184, &pm1b_control_port);
    if (pm1_event_length >= 4)
        (void)fadt_io_register(fadt, length, 56, 148, &pm1a_event_port);
    if (length >= 56 && fadt[52] && read_u32(fadt + 48) <= 0xFFFF) {
        uint16_t smi_command = (uint16_t)read_u32(fadt + 48);
        if (!(inw(pm1a_control_port) & 1)) {
            outb(smi_command, fadt[52]);
            for (uint32_t attempt = 0; attempt < 1000000; attempt++)
                if (inw(pm1a_control_port) & 1) break;
        }
    }
    if ((inw(pm1a_control_port) & 1) && facs_address && dsdt_address) {
        const uint8_t* facs = (const uint8_t*)(uintptr_t)facs_address;
        uint32_t facs_length = read_u32(facs + 4);
        if (memcmp(facs, "FACS", 4) == 0 && facs_length >= 32 &&
            aml_s3_sleep_types(dsdt_address, &pm1_sleep_type_a,
                               &pm1_sleep_type_b))
            s3_supported = 1;
    }
}

static void inspect_table(uint64_t table_address)
{
    sdt_header_t* header = (sdt_header_t*)(uintptr_t)table_address;
    if (!header || header->length < sizeof(sdt_header_t) ||
        !checksum_ok((const uint8_t*)header, header->length)) return;
    if (signature_is(header->signature, "HPET")) {
        if (header->length >= sizeof(sdt_header_t) + 20) {
            const uint8_t* gas =
                (const uint8_t*)header + sizeof(sdt_header_t) + 4;
            uint64_t base = read_u64(gas + 4);
            if (gas[0] == 0 && gas[1] >= 64 && base)
                hpet_address = base;
        }
        return;
    }
    if (signature_is(header->signature, "MCFG")) {
        if (header->length >= sizeof(sdt_header_t) + 16) {
            uint8_t* entry = (uint8_t*)header + sizeof(sdt_header_t) + 8;
            mcfg_base = *(uint64_t*)entry;
            mcfg_start_bus = entry[10];
            mcfg_end_bus = entry[11];
        }
        return;
    }
    if (signature_is(header->signature, "FACP")) {
        if (header->length >= 129) {
            uint8_t* reset = (uint8_t*)header + 116;
            uint8_t space = reset[0];
            uint8_t width = reset[1];
            uint64_t port = *(uint64_t*)(reset + 4);
            if (space == 1 && width >= 8 && port <= 0xFFFF) {
                reset_port = (uint16_t)port;
                reset_value = *((uint8_t*)header + 128);
                reset_available = 1;
            }
        }
        inspect_fadt((const uint8_t*)header, header->length);
        return;
    }
    if (!signature_is(header->signature, "APIC")) return;

    madt_address = table_address;
    madt_entries = 0;
    local_apic_address = *(uint32_t*)((uint8_t*)header + sizeof(sdt_header_t));
    ioapic_count = 0;
    first_ioapic_address = 0;
    uint8_t* entry = (uint8_t*)header + sizeof(sdt_header_t) + 8;
    uint8_t* end = (uint8_t*)header + header->length;
    while (entry + 2 <= end) {
        uint8_t length = entry[1];
        if (length < 2 || entry + length > end) break;
        if (entry[0] == 1 && length >= 12) {
            uint32_t address = *(uint32_t*)(entry + 4);
            if (ioapic_count == 0) {
                first_ioapic_address = address;
                first_ioapic_gsi_base = *(uint32_t*)(entry + 8);
            }
            ioapic_count++;
        } else if (entry[0] == 2 && length >= 10 && entry[2] == 0 &&
                   entry[3] < ACPI_MAX_ISA_IRQS) {
            uint8_t irq = entry[3];
            irq_overrides[irq].gsi = *(uint32_t*)(entry + 4);
            irq_overrides[irq].flags = *(uint16_t*)(entry + 8);
            irq_overrides[irq].valid = 1;
        }
        madt_entries++;
        entry += length;
    }
}

static void inspect_root_table(uint64_t table_address, int extended)
{
    sdt_header_t* header = (sdt_header_t*)(uintptr_t)table_address;
    if (!header || header->length < sizeof(sdt_header_t) ||
        !checksum_ok((const uint8_t*)header, header->length)) return;
    uint8_t* entries = (uint8_t*)header + sizeof(sdt_header_t);
    uint32_t entry_size = extended ? 8 : 4;
    uint32_t count = (header->length - sizeof(sdt_header_t)) / entry_size;
    if (count > 256) count = 256;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t table = extended ? *(uint64_t*)(entries + i * 8) :
            *(uint32_t*)(entries + i * 4);
        if (table) inspect_table(table);
    }
}

static int scan(uintptr_t start, uintptr_t end)
{
    for (uintptr_t candidate = start; candidate + 20 <= end; candidate += 16) {
        rsdp_t* rsdp = (rsdp_t*)candidate;
        if (rsdp->signature != RSDP_SIGNATURE) continue;
        if (!checksum_ok((const uint8_t*)rsdp, 20)) continue;
        if (rsdp->revision >= 2) {
            if (rsdp->length < 36 || candidate + rsdp->length > end ||
                !checksum_ok((const uint8_t*)rsdp, rsdp->length)) continue;
        }
        address = candidate;
        revision = rsdp->revision;
        found = 1;
        return 1;
    }
    return 0;
}

int acpi_init(void)
{
    uint16_t ebda_segment = *(volatile uint16_t*)0x40E;
    uintptr_t ebda = (uintptr_t)ebda_segment << 4;
    found = 0;
    revision = 0;
    address = 0;
    madt_address = 0;
    madt_entries = 0;
    local_apic_address = 0;
    ioapic_count = 0;
    first_ioapic_address = 0;
    first_ioapic_gsi_base = 0;
    for (uint32_t i = 0; i < ACPI_MAX_ISA_IRQS; i++)
        irq_overrides[i].valid = 0;
    mcfg_base = 0;
    mcfg_start_bus = 0;
    mcfg_end_bus = 0;
    hpet_address = 0;
    reset_port = 0;
    reset_value = 0;
    reset_available = 0;
    facs_address = 0;
    dsdt_address = 0;
    pm1a_control_port = 0;
    pm1b_control_port = 0;
    pm1a_event_port = 0;
    pm1_event_length = 0;
    pm1_sleep_type_a = 0;
    pm1_sleep_type_b = 0;
    s3_supported = 0;
    if (!(ebda >= 0x80000 && ebda < 0xA0000 && scan(ebda, ebda + 1024)) &&
        !scan(0xE0000, 0x100000)) return 0;
    rsdp_t* rsdp = (rsdp_t*)(uintptr_t)address;
    if (rsdp->revision >= 2 && rsdp->xsdt_address)
        inspect_root_table(rsdp->xsdt_address, 1);
    else if (rsdp->rsdt_address)
        inspect_root_table(rsdp->rsdt_address, 0);
    serial_puts(s3_supported ?
                "[ACPI] S3 tables and wake vector supported\n" :
                "[ACPI] S3 unavailable: firmware tables lack usable sleep state\n");
    return 1;
}

int acpi_available(void) { return found; }
uint8_t acpi_revision(void) { return revision; }
uint64_t acpi_rsdp_address(void) { return address; }
int acpi_madt_available(void) { return madt_address != 0; }
uint64_t acpi_madt_address(void) { return madt_address; }
uint32_t acpi_madt_entry_count(void) { return madt_entries; }
uint32_t acpi_local_apic_address(void) { return local_apic_address; }
uint32_t acpi_ioapic_count(void) { return ioapic_count; }
uint32_t acpi_first_ioapic_address(void) { return first_ioapic_address; }
uint32_t acpi_first_ioapic_gsi_base(void) { return first_ioapic_gsi_base; }
int acpi_irq_override(uint8_t irq, uint32_t* gsi, uint16_t* flags)
{
    if (irq >= ACPI_MAX_ISA_IRQS || !irq_overrides[irq].valid ||
        !gsi || !flags)
        return 0;
    *gsi = irq_overrides[irq].gsi;
    *flags = irq_overrides[irq].flags;
    return 1;
}
int acpi_mcfg_available(void)
{
    /* The current identity map only covers the low 1 GiB. */
    return mcfg_base != 0 && mcfg_base < 0x40000000ULL &&
           mcfg_end_bus >= mcfg_start_bus;
}
uint64_t acpi_mcfg_base(void) { return mcfg_base; }
uint8_t acpi_mcfg_start_bus(void) { return mcfg_start_bus; }
uint8_t acpi_mcfg_end_bus(void) { return mcfg_end_bus; }
int acpi_hpet_available(void) { return hpet_address != 0; }
uint64_t acpi_hpet_address(void) { return hpet_address; }

int acpi_s3_available(void)
{
    uintptr_t template_size =
        (uintptr_t)s3_wake_template_end - (uintptr_t)s3_wake_template_start;
    return s3_supported && template_size > 0 && template_size < 0x8000 &&
           pm1a_control_port != 0 && facs_address != 0;
}

void acpi_s3_resume_after_wake(void)
{
    tss_init((uint64_t)(uintptr_t)&stack_top);
    tss_load();
    timer_init();
    syscall_resume_init();
    interrupts_init();
}

int acpi_suspend_s3(void)
{
    volatile uint8_t* facs;
    uintptr_t template_size;
    uint32_t legacy_waking_vector;
    uint32_t old_waking_vector;
    uint32_t facs_flags;
    uint64_t old_x_waking_vector;
    uint64_t extended_waking_vector;
    uint16_t sleep_value;

    if (!acpi_s3_available()) return 0;
    template_size =
        (uintptr_t)s3_wake_template_end - (uintptr_t)s3_wake_template_start;
    memcpy((void*)(uintptr_t)S3_WAKE_PHYSICAL, s3_wake_template_start,
           template_size);
    legacy_waking_vector = S3_WAKE_PHYSICAL;

    facs = (volatile uint8_t*)(uintptr_t)facs_address;
    old_waking_vector = *(volatile uint32_t*)(facs + 12);
    old_x_waking_vector = *(volatile uint64_t*)(facs + 24);
    facs_flags = *(volatile uint32_t*)(facs + 20);
    extended_waking_vector = facs_flags & 1
        ? S3_WAKE_PHYSICAL +
          (uint64_t)(s3_wake_64_entry - s3_wake_template_start)
        : 0;
    *(volatile uint32_t*)(facs + 12) = legacy_waking_vector;
    *(volatile uint64_t*)(facs + 24) = extended_waking_vector;

    if (pm1a_event_port && pm1_event_length >= 4) {
        outw(pm1a_event_port, ACPI_PM1_WAKE_STATUS);
        uint16_t enabled = inw((uint16_t)(pm1a_event_port + 2));
        outw((uint16_t)(pm1a_event_port + 2),
             enabled | ACPI_PM1_POWER_BUTTON_ENABLE);
    }

    sleep_value = inw(pm1a_control_port);
    sleep_value &= (uint16_t)~ACPI_PM1_SLEEP_TYPE_MASK;
    sleep_value |= (uint16_t)(pm1_sleep_type_a << 10);
    sleep_value |= ACPI_PM1_SLEEP_ENABLE;
    uint16_t sleep_value_b = 0;
    if (pm1b_control_port) {
        sleep_value_b = inw(pm1b_control_port);
        sleep_value_b &= (uint16_t)~ACPI_PM1_SLEEP_TYPE_MASK;
        sleep_value_b |= (uint16_t)(pm1_sleep_type_b << 10);
        sleep_value_b |= ACPI_PM1_SLEEP_ENABLE;
    }
    acpi_arch_s3_enter(pm1a_control_port, pm1b_control_port, sleep_value,
                       sleep_value_b);

    *(volatile uint32_t*)(facs + 12) = old_waking_vector;
    *(volatile uint64_t*)(facs + 24) = old_x_waking_vector;
    return 1;
}

void acpi_idle_configure(void)
{
    char config[64];
    uint32_t size = 0;
    uint32_t minutes = 0;

    idle_suspend_ticks = 0;
    idle_suspend_active = 0;
    last_activity_tick = g_ticks;
    if (!acpi_s3_available()) {
        serial_puts("[ACPI] Automatic S3 suspend unavailable on this system\n");
        return;
    }
    if (!fs_load_file("/etc/power.conf", (uint8_t*)config,
                      sizeof(config) - 1, &size) ||
        size >= sizeof(config)) {
        serial_puts("[ACPI] ERROR: cannot read /etc/power.conf; suspend disabled\n");
        return;
    }
    config[size] = 0;
    if (strncmp(config, "idle_minutes=", 13) != 0) goto invalid_config;
    for (uint32_t i = 13; i < size && config[i] >= '0' && config[i] <= '9'; i++) {
        uint32_t digit = (uint32_t)(config[i] - '0');
        if (minutes > (30U - digit) / 10U) goto invalid_config;
        minutes = minutes * 10U + digit;
        if (minutes > 30) goto invalid_config;
        if (config[i + 1] == '\n' && i + 2 == size) {
            if (minutes != 0 && minutes != 5 && minutes != 10 &&
                minutes != 15 && minutes != 30)
                goto invalid_config;
            idle_suspend_ticks = (uint64_t)minutes * 60 * TIMER_HZ;
            idle_suspend_active = minutes != 0;
#ifdef ACPI_S3_IDLE_SELFTEST
            idle_suspend_ticks = 1;
            idle_suspend_active = 1;
#endif
            last_activity_tick = g_ticks;
            serial_puts("[ACPI] Automatic S3 suspend policy loaded\n");
            return;
        }
    }
invalid_config:
    serial_puts("[ACPI] ERROR: invalid /etc/power.conf; suspend disabled\n");
}

void acpi_idle_activity(void)
{
    last_activity_tick = g_ticks;
}

int acpi_idle_set_timeout(uint32_t minutes)
{
    if (!acpi_s3_available() ||
        (minutes != 0 && minutes != 5 && minutes != 10 &&
         minutes != 15 && minutes != 30))
        return 0;
    idle_suspend_ticks = (uint64_t)minutes * 60 * TIMER_HZ;
    idle_suspend_active = minutes != 0;
    last_activity_tick = g_ticks;
    return 1;
}

void acpi_idle_poll(void)
{
    uint64_t now = g_ticks;
    if (!idle_suspend_active || !idle_suspend_ticks ||
        now < last_activity_tick ||
        now - last_activity_tick < idle_suspend_ticks)
        return;
    idle_suspend_active = 0;
#ifdef ACPI_S3_IDLE_SELFTEST
    serial_puts("[s3-idle-test] ENTERING S3\n");
#endif
    serial_puts("[ACPI] Entering S3 after configured input inactivity\n");
    if (!acpi_suspend_s3()) {
#ifdef ACPI_S3_IDLE_SELFTEST
        serial_puts("[s3-idle-test] FAIL: automatic S3 entry rejected\n");
#endif
        serial_puts("[ACPI] ERROR: S3 suspend attempt failed; automatic suspend disabled\n");
        idle_suspend_ticks = 0;
        last_activity_tick = g_ticks;
        return;
    }
#ifdef ACPI_S3_IDLE_SELFTEST
    serial_puts("[s3-idle-test] RESUMED FROM S3\n");
#endif
    last_activity_tick = g_ticks;
    idle_suspend_active = idle_suspend_ticks != 0;
}

void acpi_reboot(void)
{
    if (reset_available) {
        outb(reset_port, reset_value);
        for (;;) __asm__ volatile("hlt");
    }
    for (uint32_t i = 0; i < 100000; i++)
        if (!(inb(0x64) & 2)) break;
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile("hlt");
}
