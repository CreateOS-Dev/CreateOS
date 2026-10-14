// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef KERNEL_FONT5X7_H
#define KERNEL_FONT5X7_H

#include "fonts/font8x8.h"

/* Compact view of the same bitmap glyph set: six-pixel cells and seven active rows. */
static const bitmap_font_t font5x7 = {
    "5x7", 6, 8, 32, 127, font8x8_glyphs, font8x8_lowercase,
    128, 143, font8x8_extended
};

#endif
