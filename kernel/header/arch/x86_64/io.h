#ifndef CREATEOS_ARCH_X86_64_IO_H
#define CREATEOS_ARCH_X86_64_IO_H

#if !defined(__x86_64__)
#error "CreateOS x86-64 I/O helpers require x86-64 mode"
#endif

#include "arch/x86/io.h"

static inline uint64_t mmio_read64(const volatile void* address)
{
    uint64_t value = *(const volatile uint64_t*)address;
    __asm__ volatile("" : : : "memory");
    return value;
}

static inline void mmio_write64(volatile void* address, uint64_t value)
{
    __asm__ volatile("" : : : "memory");
    *(volatile uint64_t*)address = value;
}

#endif
