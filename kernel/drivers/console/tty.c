// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "drivers/tty.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/usb.h"
#include "drivers/netstack.h"
#include "drivers/timer.h"
#include "drivers/serial.h"
#include "drivers/vga.h"
#include "drivers/framebuffer.h"
#include "drivers/acpi.h"
#include "lib/string.h"
#include <stddef.h>

#define TTY_CELLS (80 * 25)

static int session_active[TTY_CONSOLE_COUNT];
static int fullscreen_active[TTY_CONSOLE_COUNT];
static int serial_console;
static int current_console;
static int execution_console = -1;
static char console_names[TTY_CONSOLE_COUNT][16];
static char console_cwd[TTY_CONSOLE_COUNT][TTY_CWD_MAX];

static uint16_t console_buffers[TTY_CONSOLE_COUNT][TTY_CELLS];
static int console_cursor_x[TTY_CONSOLE_COUNT];
static int console_cursor_y[TTY_CONSOLE_COUNT];
static int console_output_column[TTY_CONSOLE_COUNT];
static int console_output_row[TTY_CONSOLE_COUNT];
static int serial_output_column;
static char serial_deferred_input;
static int shell_input_active;
static int ctrl_c_interrupt_enabled[TTY_CONSOLE_COUNT];

static void tty_fallback_from_serial(void)
{
    if (serial_console && !serial_is_ready())
        serial_console = 0;
}
static int mouse_x = 40;
static int mouse_y = 12;
static int mouse_remainder_x;
static int mouse_remainder_y;
static int mouse_drawn;
static int mouse_cell;
static uint16_t mouse_saved_cell;
static uint8_t mouse_buttons;
static uint64_t usb_last_hotplug_poll;
static uint8_t output_utf8_remaining[TTY_CONSOLE_COUNT + 1];

static uint32_t tty_columns(void)
{
    uint32_t columns = SCREEN_WIDTH;
    if (!serial_console && framebuffer_ready())
        framebuffer_text_dimensions(&columns, 0);
    return columns ? columns : SCREEN_WIDTH;
}

static uint32_t tty_rows(void)
{
    uint32_t rows = SCREEN_HEIGHT;
    if (!serial_console && framebuffer_ready())
        framebuffer_text_dimensions(0, &rows);
    return rows ? rows : SCREEN_HEIGHT;
}

static void tty_track_output_cell(void)
{
    int* column = serial_console ? &serial_output_column :
                  &console_output_column[current_console];
    if (++*column >= (int)tty_columns()) {
        *column = 0;
        if (!serial_console &&
            ++console_output_row[current_console] >= (int)tty_rows())
            console_output_row[current_console] = (int)tty_rows() - 1;
    }
}

static void tty_track_output(char c)
{
    uint32_t decoder = serial_console ? TTY_CONSOLE_COUNT :
                       (uint32_t)current_console;
    uint8_t byte = (uint8_t)c;
    if (decoder > TTY_CONSOLE_COUNT)
        decoder = 0;
    if (byte == '\n' || byte == '\r') {
        output_utf8_remaining[decoder] = 0;
        if (byte == '\n' &&
            !serial_console &&
            ++console_output_row[current_console] >= (int)tty_rows())
            console_output_row[current_console] = (int)tty_rows() - 1;
        int* column = serial_console ? &serial_output_column :
                      &console_output_column[current_console];
        *column = 0;
        return;
    }
    if (byte == '\b' || byte == 127) {
        output_utf8_remaining[decoder] = 0;
        int* column = serial_console ? &serial_output_column :
                      &console_output_column[current_console];
        if (*column > 0) (*column)--;
        return;
    }
    if (output_utf8_remaining[decoder]) {
        if ((byte & 0xC0) == 0x80) {
            output_utf8_remaining[decoder]--;
            return;
        }
        output_utf8_remaining[decoder] = 0;
    }
    if (byte >= 0xC2 && byte <= 0xDF)
        output_utf8_remaining[decoder] = 1;
    else if (byte >= 0xE0 && byte <= 0xEF)
        output_utf8_remaining[decoder] = 2;
    else if (byte >= 0xF0 && byte <= 0xF4)
        output_utf8_remaining[decoder] = 3;
    tty_track_output_cell();
}

static int tty_kernel_output_suppressed(void)
{
    int console = tty_execution_console();
    return !serial_console && framebuffer_ready() &&
           console >= 0 && console < TTY_CONSOLE_COUNT &&
           fullscreen_active[console];
}

static void save_console(int id)
{
    int x, y;
    vga_get_cursor(&x, &y);
    console_cursor_x[id] = x;
    console_cursor_y[id] = y;
    console_output_column[id] = x;
    console_output_row[id] = y;
    for (int i = 0; i < TTY_CELLS; i++)
        console_buffers[id][i] = vga_read_cell(i);
}

static void restore_console(int id)
{
    for (int i = 0; i < TTY_CELLS; i++)
        vga_write_cell(i, console_buffers[id][i]);
    vga_set_cursor(console_cursor_x[id], console_cursor_y[id]);
    console_output_column[id] = console_cursor_x[id];
    console_output_row[id] = console_cursor_y[id];
}

static void mouse_cursor_restore(void)
{
    if (!mouse_drawn) return;
    if (mouse_cell >= 0 && mouse_cell < TTY_CELLS)
        vga_write_cell(mouse_cell, mouse_saved_cell);
    mouse_drawn = 0;
    usb_last_hotplug_poll = 0;
}

static void mouse_cursor_draw(void)
{
    if (framebuffer_ready()) {
        framebuffer_draw_pointer(mouse_x, mouse_y);
        return;
    }
    mouse_cell = mouse_y * SCREEN_WIDTH + mouse_x;
    mouse_saved_cell = vga_read_cell(mouse_cell);
    vga_write_cell(mouse_cell, (uint16_t)(mouse_saved_cell ^ 0x7700));
    mouse_drawn = 1;
}

static void mouse_update_cursor(const mouse_event_t* event)
{
    uint32_t columns, rows;
    int delta_x, delta_y;
    if (!event || serial_console) return;
    mouse_cursor_restore();
    columns = tty_columns();
    rows = tty_rows();
    mouse_remainder_x += event->dx;
    mouse_remainder_y += event->dy;
    delta_x = mouse_remainder_x / 2;
    delta_y = mouse_remainder_y / 4;
    mouse_remainder_x %= 2;
    mouse_remainder_y %= 4;
    mouse_x += delta_x;
    mouse_y += delta_y;
    if (mouse_x < 0) {
        mouse_x = 0;
        mouse_remainder_x = 0;
    }
    if (mouse_y < 0) {
        mouse_y = 0;
        mouse_remainder_y = 0;
    }
    if (columns && (uint32_t)mouse_x >= columns) {
        mouse_x = (int)columns - 1;
        mouse_remainder_x = 0;
    }
    if (rows && (uint32_t)mouse_y >= rows) {
        mouse_y = (int)rows - 1;
        mouse_remainder_y = 0;
    }
    mouse_cursor_draw();
}

uint32_t tty_mouse_position(void)
{
    return ((uint32_t)mouse_y << 16) | (uint32_t)mouse_x;
}

void tty_init(void)
{
    serial_console = 0;
    current_console = 0;
    execution_console = -1;
    for (int console = 0; console < TTY_CONSOLE_COUNT; console++) {
        session_active[console] = 0;
        fullscreen_active[console] = 0;
        ctrl_c_interrupt_enabled[console] = 1;
        console_names[console][0] = 't';
        console_names[console][1] = 't';
        console_names[console][2] = 'y';
        console_names[console][3] = (char)('1' + console);
        console_names[console][4] = 0;
        console_cwd[console][0] = '/';
        console_cwd[console][1] = 0;
        console_cursor_x[console] = 0;
        console_cursor_y[console] = 0;
        console_output_column[console] = 0;
        for (int i = 0; i < TTY_CELLS; i++)
            console_buffers[console][i] = 0x0720;
    }
    serial_output_column = 0;
    serial_deferred_input = 0;
    vga_fix_cursor_color();
    mouse_drawn = 0;
    mouse_buttons = 0;
}

void tty_select_serial(int enabled)
{
    serial_console = enabled != 0 && serial_is_ready();
}

int tty_current_console(void) { return current_console; }

int tty_execution_console(void)
{
    if (execution_console >= 0 && execution_console < TTY_CONSOLE_COUNT)
        return execution_console;
    return current_console;
}

void tty_bind_execution_console(void)
{
    if (execution_console < 0)
        execution_console = current_console;
}

int tty_console_count(void) { return TTY_CONSOLE_COUNT; }

int tty_get_info(int console, tty_info_t* info)
{
    if (!info || console < 0 || console >= TTY_CONSOLE_COUNT)
        return -1;
    memset(info, 0, sizeof(*info));
    strncpy(info->name, console_names[console], sizeof(info->name) - 1);
    info->index = (uint32_t)console;
    if (session_active[console]) info->flags |= TTY_FLAG_ACTIVE;
    if (console == TTY_CONSOLE_COUNT - 1) info->flags |= TTY_FLAG_GRAPHICAL;
    info->width = tty_columns();
    info->height = tty_rows();
    return 0;
}

int tty_set_console_name(int console, const char* name)
{
    if (console < 0 || console >= TTY_CONSOLE_COUNT || !name)
        return -1;
    strncpy(console_names[console], name, sizeof(console_names[console]) - 1);
    console_names[console][sizeof(console_names[console]) - 1] = 0;
    return 0;
}

int tty_activate_by_name(const char* name)
{
    if (!name) return -1;
    for (int i = 0; i < TTY_CONSOLE_COUNT; i++) {
        if (!strcmp(console_names[i], name)) {
            if (i != tty_execution_console()) return -1;
            return tty_select_console(i);
        }
    }
    return -1;
}

const char* tty_name_for_console(int console)
{
    if (console < 0 || console >= TTY_CONSOLE_COUNT)
        return "tty0";
    return console_names[console];
}

int tty_switch_console(void)
{
    return tty_select_console((current_console + 1) % TTY_CONSOLE_COUNT);
}

int tty_select_console(int console)
{
    if (serial_console || console < 0 || console >= TTY_CONSOLE_COUNT)
        return current_console;
    if (console == current_console)
        return current_console;

    save_console(current_console);
    current_console = console;
    restore_console(current_console);
    if (framebuffer_ready())
        framebuffer_select_console(current_console);
    return current_console;
}

char tty_pollchar(void)
{
    mouse_event_t mouse_event;
    tty_fallback_from_serial();
    if (serial_console) {
        acpi_idle_poll();
        if (g_ticks - usb_last_hotplug_poll >= TIMER_HZ / 10) {
            usb_last_hotplug_poll = g_ticks;
            usb_poll_hotplug();
            net_stack_poll();
        }
        char c = serial_deferred_input;
        serial_deferred_input = 0;
        if (!c)
            c = serial_getc();
        if (!c) return 0;
        int console = tty_execution_console();
        if (((unsigned char)c == 3 || (unsigned char)c == KEY_CTRL_C) &&
            console >= 0 && console < TTY_CONSOLE_COUNT &&
            ctrl_c_interrupt_enabled[console])
            c = (char)KEY_INTERRUPT;
        acpi_idle_activity();
        return c;
    }
    acpi_idle_poll();
    if (g_ticks - usb_last_hotplug_poll >= TIMER_HZ / 10) {
        usb_last_hotplug_poll = g_ticks;
        usb_poll_hotplug();
        net_stack_poll();
    }
    if (mouse_poll_event(&mouse_event)) {
        uint8_t pressed = mouse_event.buttons & (uint8_t)~mouse_buttons;
        uint8_t released = mouse_buttons & (uint8_t)~mouse_event.buttons;
        int moved = mouse_event.dx || mouse_event.dy;
        mouse_update_cursor(&mouse_event);
        mouse_buttons = mouse_event.buttons;
        if (moved || pressed || released || mouse_event.wheel)
            acpi_idle_activity();
        if (pressed & 1) return (char)KEY_MOUSE_LEFT;
        if (released & 1) return (char)KEY_MOUSE_RELEASE;
        if (mouse_event.wheel) {
            int console = tty_execution_console();
            if (!fullscreen_active[console] && framebuffer_ready())
                framebuffer_scroll(mouse_event.wheel > 0 ? 3 : -3);
            return (char)(mouse_event.wheel > 0 ?
                          KEY_MOUSE_UP : KEY_MOUSE_DOWN);
        }
        if (moved)
            return (char)((mouse_event.buttons & 1) ?
                          KEY_MOUSE_DRAG : KEY_MOUSE_MOVE);
    }

    char c = (char)keyboard_poll_char();
    if (!c) return 0;
    acpi_idle_activity();
    if ((unsigned char)c == KEY_CTRL_C && !shell_input_active &&
        ctrl_c_interrupt_enabled[tty_execution_console()])
        c = (char)KEY_INTERRUPT;
    if ((unsigned char)c == KEY_SCROLL_UP ||
        (unsigned char)c == KEY_SCROLL_DOWN) {
        if (framebuffer_ready())
            framebuffer_scroll_page((unsigned char)c == KEY_SCROLL_UP ? 1 : -1);
        return 0;
    }
    if ((unsigned char)c == KEY_TTY1 || (unsigned char)c == KEY_TTY2 ||
        (unsigned char)c == KEY_TTY3 || (unsigned char)c == KEY_TTY4 ||
        (unsigned char)c == KEY_TTY5) {
        tty_select_console((unsigned char)c - KEY_TTY1);
        return 0;
    }
    if ((unsigned char)c == KEY_CTRL_TAB) {
        int console = tty_execution_console();
        if (fullscreen_active[console])
            return c;
        tty_switch_console();
        return 0;
    }
    if (current_console != tty_execution_console())
        return 0;
    return c;
}

char tty_getchar(void)
{
    char c;
    while (!(c = tty_pollchar()))
        __asm__ volatile("pause");
    return c;
}

void tty_putchar(char c, uint8_t color)
{
    tty_fallback_from_serial();
    if (tty_kernel_output_suppressed()) return;
    if (serial_console && c == '\n') {
        serial_putc('\r');
        serial_putc('\n');
    }
    else if (serial_console) serial_putc(c);
    else if (framebuffer_ready()) {
        framebuffer_putc(c, color);
    } else {
        vga_put_char_auto(c, color);
    }

    tty_track_output(c);
}

void tty_putchar_user(char c, uint8_t color)
{
    tty_fallback_from_serial();
    if (serial_console) {
        serial_putc(c == '\n' ? '\r' : c);
        if (c == '\n') serial_putc('\n');
    } else if (framebuffer_ready()) {
        framebuffer_putc(c, color);
    } else {
        vga_put_char_auto(c, color);
    }

    tty_track_output(c);
}

static void tty_write_internal(const char* text, uint8_t color, int user)
{
    tty_fallback_from_serial();
    if (!text || (!user && tty_kernel_output_suppressed())) return;
    if (serial_console)
        serial_puts(text);
    else if (framebuffer_ready())
        framebuffer_write(text, color);
    else
        vga_print(text, color);

    for (; *text; text++)
        tty_track_output(*text);
}

void tty_write(const char* text, uint8_t color)
{
    tty_write_internal(text, color, 0);
}

void tty_write_user(const char* text, uint8_t color)
{
    tty_write_internal(text, color, 1);
}

void tty_fullscreen_begin(void)
{
    int console = tty_execution_console();
    if (serial_console || !framebuffer_ready() ||
        console < 0 || console >= TTY_CONSOLE_COUNT)
        return;
    fullscreen_active[console] = 1;
    tty_clear();
}

void tty_fullscreen_end(void)
{
    int console = tty_execution_console();
    if (console < 0 || console >= TTY_CONSOLE_COUNT) return;
    if (!fullscreen_active[console]) return;
    fullscreen_active[console] = 0;
    if (!serial_console && framebuffer_ready()) tty_clear();
}

void tty_clear(void)
{
    if (serial_console) {
        serial_puts("\033[2J\033[H");
        serial_output_column = 0;
        return;
    }
    console_output_column[current_console] = 0;
    console_output_row[current_console] = 0;
    if (framebuffer_ready()) {
        framebuffer_clear();
        return;
    }
    vga_clear();
}

int tty_set_cursor(int column, int row)
{
    if (serial_console) {
        if (column < 0 || column >= SCREEN_WIDTH ||
            row < 0 || row >= SCREEN_HEIGHT)
            return -1;
        char sequence[] = "\033[00;00H";
        sequence[2] = (char)('0' + (row + 1) / 10);
        sequence[3] = (char)('0' + (row + 1) % 10);
        sequence[5] = (char)('0' + (column + 1) / 10);
        sequence[6] = (char)('0' + (column + 1) % 10);
        serial_puts(sequence);
        serial_output_column = column;
        return 0;
    }
    if (column < 0 || (uint32_t)column >= tty_columns() ||
        row < 0 || (uint32_t)row >= tty_rows())
        return -1;
    console_output_column[current_console] = column;
    if (framebuffer_ready()) {
        console_output_row[current_console] = row;
        if (!framebuffer_set_text_cursor((uint32_t)column, (uint32_t)row))
            return -1;
        return 0;
    }
    vga_set_cursor(column, row);
    console_output_row[current_console] = row;
    return 0;
}

int tty_cursor_position(void)
{
    int x = 0;
    int y = 0;
    if (serial_console)
        return -1;
    if (!framebuffer_ready())
        vga_get_cursor(&x, &y);
    else {
        x = console_output_column[current_console];
        y = console_output_row[current_console];
    }
    return (y << 16) | (x & 0xFFFF);
}

void tty_shell_input(int enabled)
{
    shell_input_active = enabled != 0;
}

void tty_set_ctrl_c_interrupt(int enabled)
{
    int console = tty_execution_console();
    if (console >= 0 && console < TTY_CONSOLE_COUNT)
        ctrl_c_interrupt_enabled[console] = enabled != 0;
}

int tty_take_ctrl_c_interrupt(void)
{
    int console = tty_execution_console();
    if (shell_input_active || console < 0 ||
        console >= TTY_CONSOLE_COUNT ||
        !ctrl_c_interrupt_enabled[console])
        return 0;
    if (serial_console) {
        if (serial_deferred_input)
            return 0;
        char input = serial_getc();
        if ((unsigned char)input == 3 ||
            (unsigned char)input == KEY_CTRL_C)
            return input != 0;
        if (input)
            serial_deferred_input = input;
        return 0;
    }
    return keyboard_take_ctrl_c();
}

void tty_newline_if_needed(void)
{
    int column = serial_console ? serial_output_column :
                 console_output_column[current_console];
    if (column) tty_putchar('\n', 0x07);
}

const char* tty_name(void)
{
    if (serial_console) return "ttyS0";
    return tty_name_for_console(tty_execution_console());
}

int tty_session_active(void)
{
    int console = serial_console ? 0 : tty_execution_console();
    return session_active[console];
}

static int tty_cwd_valid(const char* path)
{
    size_t length = 0;
    size_t component_start = 1;

    if (!path || path[0] != '/') return 0;
    while (length < TTY_CWD_MAX && path[length]) length++;
    if (length == 0 || length >= TTY_CWD_MAX) return 0;
    if (length == 1) return 1;

    for (size_t i = 1; i <= length; i++) {
        unsigned char c = (unsigned char)path[i];
        if (i < length && c != '/') {
            if (c < 0x20 || c == 0x7F) return 0;
            continue;
        }

        size_t component_length = i - component_start;
        if (component_length == 0 ||
            (component_length == 1 && path[component_start] == '.') ||
            (component_length == 2 && path[component_start] == '.' &&
             path[component_start + 1] == '.'))
            return 0;
        component_start = i + 1;
    }
    return 1;
}

void tty_set_session_active(int active)
{
    int console = serial_console ? 0 : tty_execution_console();
    session_active[console] = active != 0;
}

const char* tty_get_cwd(void)
{
    int console = serial_console ? 0 : tty_execution_console();
    return console_cwd[console];
}

int tty_set_cwd(const char* path)
{
    int console = serial_console ? 0 : tty_execution_console();
    if (!tty_cwd_valid(path)) return 0;
    strncpy(console_cwd[console], path, TTY_CWD_MAX - 1);
    console_cwd[console][TTY_CWD_MAX - 1] = 0;
    return 1;
}