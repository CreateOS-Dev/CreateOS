// ============================================================
// CreateOS 0.0.1
// ============================================================


#include <stdint.h>
#include "drivers/keyboard.h"
#include "drivers/tty.h"
#include "arch/x86_64/io.h"
#include "fs/fs.h"
#include "lib/string.h"
#include <stddef.h>

static unsigned char layout_normal[] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 127,
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,0,0,0
};

static unsigned char layout_shifted[] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 127,
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,0,0,0
};
static unsigned char console_layout_normal[TTY_CONSOLE_COUNT][sizeof(layout_normal)];
static unsigned char console_layout_shifted[TTY_CONSOLE_COUNT][sizeof(layout_shifted)];

volatile uint8_t shift_pressed = 0;
volatile uint8_t ctrl_pressed = 0;
volatile uint8_t alt_pressed = 0;
volatile uint8_t caps_lock = 0;
static uint8_t is_extended = 0;
static volatile uint8_t enter_pressed;
static volatile unsigned char key_queue[128];
static volatile uint8_t key_head;
static volatile uint8_t key_tail;

static int layout_hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int layout_parse_decimal(const char** cursor, unsigned* value)
{
    unsigned result = 0;
    const char* p = *cursor;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p - '0');
        if (result > (255U - digit) / 10U) return 0;
        result = result * 10U + digit;
        p++;
    }
    *cursor = p;
    *value = result;
    return 1;
}

static int keyboard_parse_layout(const char* path,
                                 unsigned char* next_normal,
                                 unsigned char* next_shifted)
{
    char contents[2048];
    uint32_t size = 0;
    int loaded = 0;
    if (!path || !fs_load_file(path, (uint8_t*)contents,
                                sizeof(contents) - 1, &size))
        return 0;
    if (size >= sizeof(contents))
        return 0;
    contents[size] = 0;
    memcpy(next_normal, layout_normal, sizeof(layout_normal));
    memcpy(next_shifted, layout_shifted, sizeof(layout_shifted));

    for (char* line = contents; *line;) {
        char* end = strchr(line, '\n');
        if (end) *end = 0;
        while (*line == ' ' || *line == '\t' || *line == '\r') line++;
        size_t line_length = strlen(line);
        while (line_length &&
               (line[line_length - 1] == ' ' || line[line_length - 1] == '\t' ||
                line[line_length - 1] == '\r'))
            line[--line_length] = 0;
        if (*line && *line != '#') {
            int high = layout_hex_digit(line[0]);
            int low = layout_hex_digit(line[1]);
            unsigned normal, shifted;
            unsigned code;
            const char* p;
            if (high < 0 || low < 0 || line[2] != '=')
                return 0;
            code = (unsigned)(high * 16 + low);
            p = line + 3;
            if (code >= sizeof(layout_normal) ||
                !layout_parse_decimal(&p, &normal) ||
                *p++ != ',' || !layout_parse_decimal(&p, &shifted) ||
                *p != 0 || normal >= 256 || shifted >= 256)
                return 0;
            else {
                next_normal[code] = (unsigned char)normal;
                next_shifted[code] = (unsigned char)shifted;
                loaded++;
            }
        }
        if (!end) break;
        line = end + 1;
    }
    if (!loaded) return 0;
    return loaded;
}

int keyboard_validate_layout(const char* path)
{
    unsigned char next_normal[sizeof(layout_normal)];
    unsigned char next_shifted[sizeof(layout_shifted)];
    return keyboard_parse_layout(path, next_normal, next_shifted);
}

static int keyboard_load_layout_range(const char* path, int first_console,
                                      int last_console)
{
    unsigned char next_normal[sizeof(layout_normal)];
    unsigned char next_shifted[sizeof(layout_shifted)];
    int loaded = keyboard_parse_layout(path, next_normal, next_shifted);
    uint64_t flags;
    if (!loaded) return 0;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    for (int console = first_console; console < last_console; console++) {
        memcpy(console_layout_normal[console], next_normal,
               sizeof(layout_normal));
        memcpy(console_layout_shifted[console], next_shifted,
               sizeof(layout_shifted));
    }
    if (flags & (1ULL << 9))
        __asm__ volatile("sti" ::: "memory");
    return loaded;
}

int keyboard_load_layout_for_console(const char* path, int console)
{
    if (console < 0 || console >= TTY_CONSOLE_COUNT) return 0;
    return keyboard_load_layout_range(path, console, console + 1);
}

int keyboard_load_layout(const char* path)
{
    int console = tty_current_console();
    if (!path) return 0;
    if (strncmp(path, "/etc/keyboards/", 15) == 0)
        return keyboard_load_layout_range(path, 0, TTY_CONSOLE_COUNT);
    if (console < 0 || console >= TTY_CONSOLE_COUNT) console = 0;
    return keyboard_load_layout_for_console(path, console);
}

static int keyboard_wait_input(void)
{
    for (uint32_t i = 0; i < 100000; i++)
        if (!(inb(0x64) & 2)) return 1;
    return 0;
}

static int keyboard_wait_output(void)
{
    for (uint32_t i = 0; i < 100000; i++)
        if (inb(0x64) & 1) return 1;
    return 0;
}

static void keyboard_write_command(uint8_t command)
{
    if (keyboard_wait_input()) outb(0x64, command);
}

static void keyboard_write_data(uint8_t data)
{
    if (keyboard_wait_input()) outb(0x60, data);
}

static int keyboard_read_response(uint8_t expected)
{
    for (uint32_t i = 0; i < 100000; i++) {
        if (!(inb(0x64) & 1)) continue;
        if (inb(0x60) == expected) return 1;
    }
    return 0;
}

int keyboard_recover(void)
{
    shift_pressed = 0;
    ctrl_pressed = 0;
    alt_pressed = 0;
    is_extended = 0;
    enter_pressed = 0;
    key_head = 0;
    key_tail = 0;
    while (inb(0x64) & 1) (void)inb(0x60);
    keyboard_write_data(0xFF);
    if (!keyboard_read_response(0xFA)) return 0;
    return keyboard_read_response(0xAA);
}

static void queue_key(unsigned char key)
{
    uint8_t next = (uint8_t)((key_head + 1) & 0x7F);
    if (!key || next == key_tail) return;
    key_queue[key_head] = key;
    key_head = next;
}

void keyboard_inject_char(unsigned char key)
{
    queue_key(key);
}

static unsigned char dequeue_key(void)
{
    unsigned char key;
    if (key_tail == key_head) return 0;
    key = key_queue[key_tail];
    key_tail = (uint8_t)((key_tail + 1) & 0x7F);
    return key;
}

int keyboard_take_ctrl_c(void)
{
    uint64_t flags;
    uint8_t count;
    int found = 0;

    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    count = (uint8_t)((key_head - key_tail) & 0x7F);
    for (uint8_t offset = 0; offset < count; offset++) {
        uint8_t index = (uint8_t)((key_tail + offset) & 0x7F);
        if (key_queue[index] != KEY_CTRL_C)
            continue;
        for (uint8_t move = offset; move + 1 < count; move++) {
            uint8_t destination = (uint8_t)((key_tail + move) & 0x7F);
            uint8_t source = (uint8_t)((key_tail + move + 1) & 0x7F);
            key_queue[destination] = key_queue[source];
        }
        key_head = (uint8_t)((key_head - 1) & 0x7F);
        found = 1;
        break;
    }
    if (flags & (1ULL << 9))
        __asm__ volatile("sti" ::: "memory");
    return found;
}

int keyboard_init(void) {
    for (int console = 0; console < TTY_CONSOLE_COUNT; console++) {
        memcpy(console_layout_normal[console], layout_normal,
               sizeof(layout_normal));
        memcpy(console_layout_shifted[console], layout_shifted,
               sizeof(layout_shifted));
    }
    shift_pressed = 0;
    ctrl_pressed = 0;
    alt_pressed = 0;
    caps_lock = 0;
    is_extended = 0;
    enter_pressed = 0;
    key_head = 0;
    key_tail = 0;

    /* Discard stale controller output before reading the configuration byte. */
    while (inb(0x64) & 1) (void)inb(0x60);
    keyboard_write_command(0xAD);
    keyboard_write_command(0x20);
    if (!keyboard_wait_output()) return 0;
    uint8_t config = inb(0x60);
    config |= 0x41;
    config &= (uint8_t)~0x10;
    config &= (uint8_t)~0x20;
    keyboard_write_command(0x60);
    keyboard_write_data(config);
    keyboard_write_command(0xAE);
    while (inb(0x64) & 1) (void)inb(0x60);

    keyboard_write_data(0xF4);
    if (keyboard_read_response(0xFA)) return 1;
    return keyboard_recover();
}

uint16_t keyboard_read_scancode(void) {
    uint8_t status = inb(0x64);
    if (!(status & 1) || (status & 0x20))
        return 0;

    uint8_t code = inb(0x60);

    if (code == 0xE0) {
        is_extended = 1;
        return 0;
    }

    if (is_extended) {
        is_extended = 0;
        return ((uint16_t)code) | 0xE000;
    }

    return code;
}

unsigned char scancode_to_char(uint16_t scancode, int *is_release) {
    uint8_t code = scancode & 0xFF;
    int extended = (scancode & 0xE000) == 0xE000;

    if (code & 0x80) {
        *is_release = 1;
        code &= 0x7F;
    } else {
        *is_release = 0;
    }

    if (!extended && ((code & 0xFF) == 0x2A || (code & 0xFF) == 0x36)) {
        shift_pressed = !(*is_release);
        return 0;
    }

    if (!extended && (code & 0xFF) == 0x38) {
        alt_pressed = !(*is_release);
        return 0;
    }

    if (!extended && (code & 0xFF) == 0x1D) {
        ctrl_pressed = !(*is_release);
        return 0;
    }

    if (!extended && (code & 0xFF) == 0x3A) {
        if (!(*is_release))
            caps_lock = !caps_lock;
        return 0;
    }

    if (!extended && code == 0x1C) {
        if (*is_release) {
            enter_pressed = 0;
            return 0;
        }
        if (enter_pressed)
            return 0;
        enter_pressed = 1;
        return '\n';
    }

    if (!extended && ctrl_pressed && code == 0x2E && !(*is_release))
        return KEY_CTRL_C;
    if (!extended && ctrl_pressed && code == 0x2F && !(*is_release))
        return KEY_CTRL_V;
    if (!extended && ctrl_pressed && code == 0x2D && !(*is_release))
        return KEY_CTRL_X;

    if (*is_release)
        return 0;

    if (extended) {
        if (code == SCANCODE_LEFT)
            return shift_pressed ? KEY_SHIFT_LEFT : KEY_LEFT;
        if (code == SCANCODE_RIGHT)
            return shift_pressed ? KEY_SHIFT_RIGHT : KEY_RIGHT;
        if (code == SCANCODE_UP)
            return shift_pressed ? KEY_SHIFT_UP : KEY_UP;
        if (code == SCANCODE_DOWN)
            return shift_pressed ? KEY_SHIFT_DOWN : KEY_DOWN;
        if (code == 0x49) return KEY_SCROLL_UP;
        if (code == 0x51) return KEY_SCROLL_DOWN;
        if (code == 0x53)  return KEY_DELETE;
        if (code == 0x47 && shift_pressed) return KEY_SHIFT_HOME;
        if (code == 0x4F && shift_pressed) return KEY_SHIFT_END;
        return 0;
    }

    if (code == 0x0E) return 127;
    if (ctrl_pressed && code == SCANCODE_TAB) return KEY_CTRL_TAB;
    if (code == SCANCODE_TAB) return shift_pressed ? KEY_SHIFT_TAB : KEY_TAB;

    if (alt_pressed) {
        if (code == 0x3B) return KEY_TTY1;
        if (code == 0x3C) return KEY_TTY2;
        if (code == 0x3D) return KEY_TTY3;
        if (code == 0x3E) return KEY_TTY4;
        if (code == 0x3F) return KEY_TTY5;
    }

    if (code == 0x39) return ' ';

    if (ctrl_pressed && alt_pressed) {
        if (code == 0x02) return KEY_TTY1;
        if (code == 0x03) return KEY_TTY2;
        if (code == 0x04) return KEY_TTY3;
        if (code == 0x05) return KEY_TTY4;
        if (code == 0x06) return KEY_TTY5;
    }

    int console = tty_current_console();
    if (console < 0 || console >= TTY_CONSOLE_COUNT) console = 0;
    if (code >= sizeof(layout_normal))
        return 0;

    unsigned char c = console_layout_normal[console][code];
    int is_alpha = (c >= 'a' && c <= 'z');

    if (ctrl_pressed && is_alpha)
        return (unsigned char)(c & 0x1F);
    if (shift_pressed ^ (caps_lock && is_alpha))
        return console_layout_shifted[console][code];

    return c;
}

unsigned char keyboard_poll_char(void) {
    int is_release = 0;
    unsigned char c = dequeue_key();
    if (c) return c;

    for (int i = 0; i < 16; i++) {
        uint16_t sc = keyboard_read_scancode();
        if (!sc) {
            __asm__ volatile("pause");
            continue;
        }

        c = scancode_to_char(sc, &is_release);
        if (!is_release && c) return c;
    }

    return 0;
}

unsigned char keyboard_get_char(void) {
    unsigned char c;
    while (!(c = keyboard_poll_char()))
        __asm__ volatile("pause");
    return c;
}

void keyboard_irq_handler(void)
{
    for (int i = 0; i < 16; i++) {
        uint16_t scancode = keyboard_read_scancode();
        int is_release = 0;
        if (!scancode) break;
        queue_key(scancode_to_char(scancode, &is_release));
    }
}