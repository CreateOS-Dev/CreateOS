// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef KERNEL_APIC_H
#define KERNEL_APIC_H

int apic_init(void);
int apic_available(void);
void apic_eoi(void);

#endif