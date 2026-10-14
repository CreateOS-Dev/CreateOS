// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

int keyboard_init(void);
int keyboard_recover(void);
int keyboard_load_layout(const char* path);
int keyboard_load_layout_for_console(const char* path, int console);
int keyboard_validate_layout(const char* path);
uint16_t keyboard_read_scancode(void);
unsigned char keyboard_get_char(void);
unsigned char keyboard_poll_char(void);
int keyboard_take_ctrl_c(void);
void keyboard_irq_handler(void);
void keyboard_inject_char(unsigned char key);

#define SCANCODE_BACKSPACE 0x0E
#define SCANCODE_TAB       0x0F
#define SCANCODE_ENTER     0x1C
#define SCANCODE_SHIFT     0x2A
#define SCANCODE_CAPSLOCK  0x3A
#define SCANCODE_SPACE     0x39   // ⭐ BARRE D’ESPACE ⭐

#define SCANCODE_LEFT   0x4B
#define SCANCODE_RIGHT  0x4D
#define SCANCODE_UP     0x48
#define SCANCODE_DOWN   0x50
#define SCANCODE_DELETE 0x53

#define KEY_LEFT       0x90
#define KEY_RIGHT      0x91
#define KEY_UP         0x92
#define KEY_DOWN       0x93
#define KEY_DELETE     0x94
#define KEY_TAB        0x95
#define KEY_SHIFT_TAB  0x96
#define KEY_INTERRUPT  0x97
#define KEY_TTY_NEXT   0x98
#define KEY_SCROLL_UP   0x99
#define KEY_SCROLL_DOWN 0x9A
#define KEY_MOUSE_MOVE  0x9B
#define KEY_MOUSE_LEFT  0x9C
#define KEY_MOUSE_UP    0x9D
#define KEY_MOUSE_DOWN  0x9E
#define KEY_CTRL_TAB    0x9F
#define KEY_SHIFT_LEFT  0xA0
#define KEY_SHIFT_RIGHT 0xA1
#define KEY_SHIFT_UP    0xA2
#define KEY_SHIFT_DOWN  0xA3
#define KEY_CTRL_C      0xA4
#define KEY_CTRL_V      0xA5
#define KEY_SHIFT_HOME  0xA6
#define KEY_SHIFT_END   0xA7
#define KEY_MOUSE_DRAG  0xA8
#define KEY_MOUSE_RELEASE 0xA9
#define KEY_CTRL_X      0xAA

#define KEY_TTY1   0x81
#define KEY_TTY2   0x82
#define KEY_TTY3   0x83
#define KEY_TTY4   0x84
#define KEY_TTY5   0x85

#endif
