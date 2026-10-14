#include "arch/x86_64/cpu.h"

#define CR0_MP (1ULL << 1)
#define CR0_EM (1ULL << 2)
#define CR4_OSFXSR (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)

int x86_64_cpu_init(void)
{
    uint64_t cr0;
    uint64_t cr4;

    if (!cpu_has_long_mode() || !cpu_has_msr() || !cpu_has_sse2())
        return 0;

    cr0 = cpu_x86_64_read_cr0();
    cr0 = (cr0 | CR0_MP) & ~CR0_EM;
    cpu_x86_64_write_cr0(cr0);

    cr4 = cpu_x86_64_read_cr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    cpu_x86_64_write_cr4(cr4);
    return 1;
}
