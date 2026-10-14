// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>

void interrupts_init(void);
uint64_t interrupt_dispatch(unsigned long vector, unsigned long error_code,
                            unsigned long rip, unsigned long cs,
                            unsigned long rflags);

#endif
