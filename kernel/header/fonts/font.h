// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef KERNEL_FONT_H
#define KERNEL_FONT_H

#include <stdint.h>

typedef struct {
    const char* name;
    uint8_t width;
    uint8_t height;
    uint8_t first;
    uint8_t last;
    const uint8_t* glyphs;
    const uint8_t* lowercase_glyphs;
    uint8_t extended_first;
    uint8_t extended_last;
    const uint8_t* extended_glyphs;
} bitmap_font_t;

#endif
