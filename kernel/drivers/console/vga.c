// ============================================================
// CreateOS 0.0.1
// ============================================================


#include "drivers/vga.h"
#include <stdbool.h>
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

volatile uint16_t* const VGA_ADDRESS = (volatile uint16_t*)0xB8000;

int cursor_x = 0;
int cursor_y = 0;

static uint16_t vga_entry(uint8_t c, uint8_t fg, uint8_t bg) {
    return ((uint16_t)c) | ((uint16_t)((bg << 4) | (fg & 0x0F)) << 8);
}

void vga_update_cursor(int x, int y) {
    uint16_t pos = y * 80 + x;

    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));

    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_get_cursor(int* x, int* y) {
    if (x) *x = cursor_x;
    if (y) *y = cursor_y;
}

void vga_set_cursor(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= 80) x = 79;
    if (y >= 25) y = 24;

    cursor_x = x;
    cursor_y = y;
    vga_update_cursor(cursor_x, cursor_y);
}

void vga_fix_cursor_color(void) {
    outb(0x3D4, 0x0A);
    uint8_t val = inb(0x3D5);

    val &= ~(1 << 5);
    val &= ~(1 << 6);

    outb(0x3D4, 0x0A);
    outb(0x3D5, val);
}

void vga_clear() {
    for (int i = 0; i < 80 * 25; i++)
        VGA_ADDRESS[i] = vga_entry(' ', 0x0F, 0x00);

    cursor_x = 0;
    cursor_y = 0;
    vga_update_cursor(0, 0);
}

void vga_scroll() {
    for (int i = 0; i < (25 - 1) * 80; i++)
        VGA_ADDRESS[i] = VGA_ADDRESS[i + 80];

    for (int i = (25 - 1) * 80; i < 25 * 80; i++)
        VGA_ADDRESS[i] = vga_entry(' ', 0x0F, 0x00);

    vga_update_cursor(cursor_x, cursor_y);
}

void vga_putc(char c, int x, int y, uint8_t fg) {
    unsigned char uc = (unsigned char)c;
    VGA_ADDRESS[y * 80 + x] = vga_entry(uc, fg, 0x00);
}

void vga_put_char_auto(char c, uint8_t fg) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= 25) {
            // Scroll the screen up by one line.
            for (int i = 0; i < 24 * 80; i++)
                VGA_ADDRESS[i] = VGA_ADDRESS[i + 80];
            for (int i = 24 * 80; i < 25 * 80; i++)
                VGA_ADDRESS[i] = vga_entry(' ', 0x0F, 0x00);
            cursor_y = 24;
        }
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == '\b' || (unsigned char)c == 127) {
        if (cursor_x > 0) {
            cursor_x--;
            VGA_ADDRESS[cursor_y * 80 + cursor_x] = vga_entry(' ', fg, 0x00);
        }
    } else {
        vga_putc(c, cursor_x, cursor_y, fg);
        cursor_x++;
        if (cursor_x >= 80) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y >= 25) {
                // Scroll
                for (int i = 0; i < 24 * 80; i++)
                    VGA_ADDRESS[i] = VGA_ADDRESS[i + 80];
                for (int i = 24 * 80; i < 25 * 80; i++)
                    VGA_ADDRESS[i] = vga_entry(' ', 0x0F, 0x00);
                cursor_y = 24;
            }
        }
    }
    vga_update_cursor(cursor_x, cursor_y);
}

void vga_print(const char *str, uint8_t fg) {
    while (*str) {
        vga_put_char_auto(*str++, fg);
    }
}

void vga_put_uint_auto(uint32_t num, uint8_t fg) {
    char buf[16];
    int i = 0;
    if (num == 0) {
        buf[i++] = '0';
    } else {
        while (num > 0) {
            buf[i++] = (num % 10) + '0';
            num /= 10;
        }
    }
    buf[i] = '\0';

    // Reverse the string
    for (int j = 0, k = i - 1; j < k; j++, k--) {
        char tmp = buf[j];
        buf[j] = buf[k];
        buf[k] = tmp;
    }

    vga_print(buf, fg);
}

uint16_t vga_read_cell(int index)
{
    if (index < 0 || index >= SCREEN_WIDTH * SCREEN_HEIGHT) return 0;
    return VGA_ADDRESS[index];
}

void vga_write_cell(int index, uint16_t value)
{
    if (index < 0 || index >= SCREEN_WIDTH * SCREEN_HEIGHT) return;
    VGA_ADDRESS[index] = value;
}

void vga_putint(int num, uint8_t fg) {
    char buf[16];
    int i = 0;
    bool is_negative = false;

    if (num == 0) {
        vga_print("0", fg);
        return;
    }

    if (num < 0) {
        is_negative = true;
        num = -num;
    }

    while (num > 0) {
        buf[i++] = (num % 10) + '0';
        num /= 10;
    }

    if (is_negative)
        buf[i++] = '-';

    buf[i] = '\0';

    // Reverse the string
    for (int j = 0, k = i - 1; j < k; j++, k--) {
        char tmp = buf[j];
        buf[j] = buf[k];
        buf[k] = tmp;
    }

    vga_print(buf, fg);
}