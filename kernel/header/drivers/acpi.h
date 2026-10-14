// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef KERNEL_ACPI_H
#define KERNEL_ACPI_H

#include <stdint.h>

int acpi_init(void);
int acpi_available(void);
uint8_t acpi_revision(void);
uint64_t acpi_rsdp_address(void);
int acpi_madt_available(void);
uint64_t acpi_madt_address(void);
uint32_t acpi_madt_entry_count(void);
uint32_t acpi_local_apic_address(void);
uint32_t acpi_ioapic_count(void);
uint32_t acpi_first_ioapic_address(void);
uint32_t acpi_first_ioapic_gsi_base(void);
int acpi_irq_override(uint8_t irq, uint32_t* gsi, uint16_t* flags);
int acpi_mcfg_available(void);
uint64_t acpi_mcfg_base(void);
uint8_t acpi_mcfg_start_bus(void);
uint8_t acpi_mcfg_end_bus(void);
int acpi_hpet_available(void);
uint64_t acpi_hpet_address(void);
void acpi_reboot(void);
int acpi_s3_available(void);
int acpi_suspend_s3(void);
void acpi_s3_resume_after_wake(void);
void acpi_idle_configure(void);
int acpi_idle_set_timeout(uint32_t minutes);
void acpi_idle_activity(void);
void acpi_idle_poll(void);

#endif
