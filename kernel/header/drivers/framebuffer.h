// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef KERNEL_FRAMEBUFFER_H
#define KERNEL_FRAMEBUFFER_H

#include <stdint.h>

typedef struct {
    uint64_t address;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t type;
    int available;
} framebuffer_info_t;

void framebuffer_init(const void* multiboot_info);
int framebuffer_map(void);
int framebuffer_font_init(void);
int framebuffer_ready(void);
const framebuffer_info_t* framebuffer_info(void);
void framebuffer_fill(uint32_t color);
void framebuffer_clear(void);
void framebuffer_scroll(int lines);
void framebuffer_scroll_page(int direction);
void framebuffer_select_console(int console);
int framebuffer_set_text_cursor(uint32_t column, uint32_t row);
void framebuffer_text_dimensions(uint32_t* columns, uint32_t* rows);
int framebuffer_set_pointer_sprite(const char* data, uint32_t size);
void framebuffer_set_pointer_busy(int busy);
void framebuffer_pointer_tick(uint64_t ticks);
void framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t color);
int framebuffer_font_count(void);
const char* framebuffer_font_name(int index);
int framebuffer_set_font(int index);
void framebuffer_set_scale(int scale);
int framebuffer_font(void);
void framebuffer_putc(char c, uint8_t color);
void framebuffer_write(const char* text, uint8_t color);
void framebuffer_draw_pointer(int cell_x, int cell_y);

#endif
