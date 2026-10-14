// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef KERNEL_TTY_H
#define KERNEL_TTY_H

#include <stdint.h>

#define TTY_CONSOLE_COUNT 5
#define TTY_CWD_MAX 256

#define TTY_FLAG_ACTIVE    0x01
#define TTY_FLAG_GRAPHICAL 0x02

typedef struct {
	char name[16];
	uint32_t index;
	uint32_t flags;
	uint32_t width;
	uint32_t height;
} tty_info_t;

void tty_init(void);
void tty_select_serial(int enabled);
int tty_current_console(void);
int tty_execution_console(void);
void tty_bind_execution_console(void);
int tty_switch_console(void);
int tty_select_console(int console);
int tty_console_count(void);
int tty_set_console_name(int console, const char* name);
int tty_activate_by_name(const char* name);
const char* tty_name_for_console(int console);
int tty_get_info(int console, tty_info_t* info);
char tty_getchar(void);
char tty_pollchar(void);
int tty_cursor_position(void);
void tty_shell_input(int enabled);
void tty_set_ctrl_c_interrupt(int enabled);
int tty_take_ctrl_c_interrupt(void);
void tty_putchar(char c, uint8_t color);
void tty_putchar_user(char c, uint8_t color);
void tty_write(const char* text, uint8_t color);
void tty_write_user(const char* text, uint8_t color);
void tty_clear(void);
int tty_set_cursor(int column, int row);
uint32_t tty_mouse_position(void);
void tty_fullscreen_begin(void);
void tty_fullscreen_end(void);
void tty_newline_if_needed(void);
const char* tty_name(void);
int tty_session_active(void);
void tty_set_session_active(int active);
const char* tty_get_cwd(void);
int tty_set_cwd(const char* path);

#endif