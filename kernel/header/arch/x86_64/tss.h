// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef KERNEL_TSS_H
#define KERNEL_TSS_H

#include <stdint.h>

void tss_init(uint64_t ring0_stack);
void tss_load(void);

#endif
