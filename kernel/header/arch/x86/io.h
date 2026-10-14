#ifndef CREATEOS_ARCH_X86_IO_H
#define CREATEOS_ARCH_X86_IO_H

#if !defined(__i386__) && !defined(__x86_64__)
#error "CreateOS x86 I/O helpers require an x86 target"
#endif

#include <stdint.h>
#include "arch/x86/cpu.h"

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void io_wait(void)
{
    outb(0x80, 0);
}

static inline uintptr_t irq_save(void)
{
#if defined(__x86_64__)
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return (uintptr_t)flags;
#else
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return (uintptr_t)flags;
#endif
}

static inline void irq_restore(uintptr_t flags)
{
    if (flags & (1U << 9))
        __asm__ volatile("sti" : : : "memory");
    else
        __asm__ volatile("cli" : : : "memory");
}

static inline uint8_t mmio_read8(const volatile void* address)
{
    uint8_t value = *(const volatile uint8_t*)address;
    __asm__ volatile("" : : : "memory");
    return value;
}

static inline uint16_t mmio_read16(const volatile void* address)
{
    uint16_t value = *(const volatile uint16_t*)address;
    __asm__ volatile("" : : : "memory");
    return value;
}

static inline uint32_t mmio_read32(const volatile void* address)
{
    uint32_t value = *(const volatile uint32_t*)address;
    __asm__ volatile("" : : : "memory");
    return value;
}

static inline void mmio_write8(volatile void* address, uint8_t value)
{
    __asm__ volatile("" : : : "memory");
    *(volatile uint8_t*)address = value;
}

static inline void mmio_write16(volatile void* address, uint16_t value)
{
    __asm__ volatile("" : : : "memory");
    *(volatile uint16_t*)address = value;
}

static inline void mmio_write32(volatile void* address, uint32_t value)
{
    __asm__ volatile("" : : : "memory");
    *(volatile uint32_t*)address = value;
}

static inline void io_memory_barrier(void)
{
#if defined(__x86_64__)
    __asm__ volatile("mfence" : : : "memory");
#else
    uint32_t value = 0;
    __asm__ volatile("lock; addl $0, %0" : "+m"(value) : : "memory");
#endif
}

#endif
