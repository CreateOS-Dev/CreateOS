#ifndef KERNEL_FONT_NOTO_SANS_H
#define KERNEL_FONT_NOTO_SANS_H

#include <stddef.h>
#include <stdint.h>

extern const uint8_t createos_noto_sans_ttf_start[];
extern const uint8_t createos_noto_sans_ttf_end[];

static inline size_t createos_noto_sans_ttf_size(void)
{
    return (size_t)(createos_noto_sans_ttf_end -
                    createos_noto_sans_ttf_start);
}

#endif
