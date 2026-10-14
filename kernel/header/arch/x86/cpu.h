#ifndef CREATEOS_ARCH_X86_CPU_H
#define CREATEOS_ARCH_X86_CPU_H

#if !defined(__i386__) && !defined(__x86_64__)
#error "CreateOS x86 CPU helpers require an x86 target"
#endif

#include <stdint.h>

#if defined(__x86_64__)
typedef uint64_t cpu_register_t;
#else
typedef uint32_t cpu_register_t;
#endif

typedef struct {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
} cpu_cpuid_result_t;

#define CPU_CPUID_FEATURE_MSR  (1U << 5)
#define CPU_CPUID_FEATURE_APIC (1U << 9)
#define CPU_CPUID_FEATURE_RDRAND (1U << 30)
#define CPU_CPUID_FEATURE_RDSEED (1U << 18)
#define CPU_CPUID_FEATURE_SSE2 (1U << 26)
#define CPU_CPUID_EXT_FEATURE_NX         (1U << 20)
#define CPU_CPUID_EXT_FEATURE_LONG_MODE  (1U << 29)

static inline int cpu_cpuid_supported(void)
{
#if defined(__x86_64__)
    return 1;
#else
    uintptr_t original_flags;
    uintptr_t changed_flags;
    uintptr_t observed_flags;

    __asm__ volatile("pushfl; popl %0" : "=r"(original_flags));
    changed_flags = original_flags ^ (1U << 21);
    __asm__ volatile("pushl %0; popfl" : : "r"(changed_flags) : "cc");
    __asm__ volatile("pushfl; popl %0" : "=r"(observed_flags));
    __asm__ volatile("pushl %0; popfl" : : "r"(original_flags) : "cc");
    return ((observed_flags ^ original_flags) & (1U << 21)) != 0;
#endif
}

static inline cpu_cpuid_result_t cpu_cpuid(uint32_t leaf, uint32_t subleaf)
{
    cpu_cpuid_result_t result;
    __asm__ volatile("cpuid"
                     : "=a"(result.eax), "=b"(result.ebx),
                       "=c"(result.ecx), "=d"(result.edx)
                     : "a"(leaf), "c"(subleaf)
                     : "memory");
    return result;
}

static inline uint32_t cpu_max_basic_leaf(void)
{
    return cpu_cpuid_supported() ? cpu_cpuid(0, 0).eax : 0;
}

static inline uint32_t cpu_max_extended_leaf(void)
{
    if (!cpu_cpuid_supported()) return 0;
    return cpu_cpuid(0x80000000U, 0).eax;
}

static inline int cpu_has_msr(void)
{
    return cpu_max_basic_leaf() >= 1 &&
           (cpu_cpuid(1, 0).edx & CPU_CPUID_FEATURE_MSR) != 0;
}

static inline int cpu_has_apic(void)
{
    return cpu_max_basic_leaf() >= 1 &&
           (cpu_cpuid(1, 0).edx & CPU_CPUID_FEATURE_APIC) != 0;
}

static inline int cpu_has_sse2(void)
{
    return cpu_max_basic_leaf() >= 1 &&
           (cpu_cpuid(1, 0).edx & CPU_CPUID_FEATURE_SSE2) != 0;
}

static inline int cpu_has_rdrand(void)
{
    return cpu_max_basic_leaf() >= 1 &&
           (cpu_cpuid(1, 0).ecx & CPU_CPUID_FEATURE_RDRAND) != 0;
}

static inline int cpu_has_rdseed(void)
{
    return cpu_max_basic_leaf() >= 7 &&
           (cpu_cpuid(7, 0).ebx & CPU_CPUID_FEATURE_RDSEED) != 0;
}

static inline int cpu_has_nx(void)
{
    return cpu_max_extended_leaf() >= 0x80000001U &&
           (cpu_cpuid(0x80000001U, 0).edx &
            CPU_CPUID_EXT_FEATURE_NX) != 0;
}

static inline int cpu_has_long_mode(void)
{
    return cpu_max_extended_leaf() >= 0x80000001U &&
           (cpu_cpuid(0x80000001U, 0).edx &
            CPU_CPUID_EXT_FEATURE_LONG_MODE) != 0;
}

static inline uint64_t cpu_read_msr(uint32_t index)
{
    uint32_t low;
    uint32_t high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(index));
    return ((uint64_t)high << 32) | low;
}

static inline void cpu_write_msr(uint32_t index, uint64_t value)
{
    uint32_t low = (uint32_t)value;
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(index));
}

static inline cpu_register_t cpu_read_cr2(void)
{
    cpu_register_t value;
    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

static inline cpu_register_t cpu_read_cr3(void)
{
    cpu_register_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static inline void cpu_write_cr3(cpu_register_t value)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

static inline void cpu_invalidate_page(const void* address)
{
    __asm__ volatile("invlpg (%0)" : : "r"(address) : "memory");
}

static inline void cpu_pause(void)
{
    __asm__ volatile("pause");
}

static inline void cpu_halt(void)
{
    __asm__ volatile("hlt");
}

#endif
