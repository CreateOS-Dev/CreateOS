// ============================================================
// CreateOS 0.0.1
// ============================================================


#ifndef VGA_H
#define VGA_H

#include <stdint.h>

#define SCREEN_WIDTH 80
#define SCREEN_HEIGHT 25

void vga_update_cursor(int x, int y);
void vga_get_cursor(int* x, int* y);
void vga_set_cursor(int x, int y);
void vga_fix_cursor_color(void);

void vga_clear(void);
void vga_putc(char c, int x, int y, uint8_t fg);
void vga_put_char_auto(char c, uint8_t fg);
void vga_print(const char* s, uint8_t fg);
void vga_put_uint_auto(uint32_t num, uint8_t color);
uint16_t vga_read_cell(int index);
void vga_write_cell(int index, uint16_t value);

#endif