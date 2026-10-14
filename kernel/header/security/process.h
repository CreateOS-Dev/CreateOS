#ifndef KERNEL_SECURITY_PROCESS_H
#define KERNEL_SECURITY_PROCESS_H

#include <stdint.h>

#define SECURITY_PROCESS_GENERATION_MAX 0x01FFFFFEULL

uint64_t security_process_generation(int console);
int security_process_generation_matches(int console, uint64_t generation);
int security_advance_process_generation(int console);

#endif
