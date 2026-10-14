#include "arch/x86_64/random.h"
#include "arch/x86_64/cpu.h"
#include "lib/string.h"

#define RANDOM_RETRIES 10

static int random_word(uint64_t* value, int have_rdseed, int have_rdrand)
{
    unsigned char success = 0;

    if (have_rdseed) {
        for (int attempt = 0; attempt < RANDOM_RETRIES; attempt++) {
            __asm__ volatile("rdseed %0; setc %1"
                             : "=&r"(*value), "=qm"(success)
                             :
                             : "cc");
            if (success) return 1;
        }
    }
    if (have_rdrand) {
        for (int attempt = 0; attempt < RANDOM_RETRIES; attempt++) {
            __asm__ volatile("rdrand %0; setc %1"
                             : "=&r"(*value), "=qm"(success)
                             :
                             : "cc");
            if (success) return 1;
        }
    }
    return 0;
}

int x86_64_random_bytes(void* buffer, size_t size)
{
    uint8_t* output = (uint8_t*)buffer;
    size_t offset = 0;
    int have_rdseed = cpu_has_rdseed();
    int have_rdrand = cpu_has_rdrand();

    if (!size)
        return 1;
    if (!buffer || (!have_rdseed && !have_rdrand))
        return 0;

    while (offset < size) {
        uint64_t value;
        if (!random_word(&value, have_rdseed, have_rdrand)) {
            memset(output, 0, size);
            return 0;
        }

        for (size_t byte = 0; byte < sizeof(value) && offset < size;
             byte++, offset++)
            output[offset] = (uint8_t)(value >> (byte * 8));
    }

    return 1;
}
