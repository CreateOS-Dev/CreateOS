#ifndef CREATEOS_ARCH_X86_64_CPU_H
#define CREATEOS_ARCH_X86_64_CPU_H

#if !defined(__x86_64__)
#error "CreateOS x86-64 CPU helpers require x86-64 mode"
#endif

#include "arch/x86/cpu.h"

int x86_64_cpu_init(void);

static inline uint64_t cpu_x86_64_read_cr0(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

static inline uint64_t cpu_x86_64_read_cr4(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

static inline void cpu_x86_64_write_cr0(uint64_t value)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(value) : "memory");
}

static inline void cpu_x86_64_write_cr4(uint64_t value)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(value) : "memory");
}

#endif
