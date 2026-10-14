#ifndef CREATEOS_ARCH_X86_64_RANDOM_H
#define CREATEOS_ARCH_X86_64_RANDOM_H

#include <stddef.h>

int x86_64_random_bytes(void* buffer, size_t size);

#endif
