#ifndef KERNEL_HPET_H
#define KERNEL_HPET_H

#include <stdint.h>

int hpet_init(uint64_t physical_base);
int hpet_available(void);
int hpet_monotonic_ns(uint64_t* nanoseconds);

#endif
