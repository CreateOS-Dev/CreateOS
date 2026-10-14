#include "stdio.h"
#include "stdlib.h"
#include "drivers/serial.h"
#include "createos_alloc.h"

struct createos_tinygl_file {
    int unused;
};

static struct createos_tinygl_file tinygl_stderr;
static struct createos_tinygl_file tinygl_stdout;
FILE* stderr = &tinygl_stderr;
FILE* stdout = &tinygl_stdout;

int vfprintf(FILE* stream, const char* format, va_list args)
{
    (void)stream;
    (void)args;
    if (format)
        serial_puts_raw(format);
    return 0;
}

int fprintf(FILE* stream, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfprintf(stream, format, args);
    va_end(args);
    return result;
}

int printf(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfprintf(stdout, format, args);
    va_end(args);
    return result;
}

int fputc(int character, FILE* stream)
{
    char text[2] = {(char)character, 0};
    (void)stream;
    serial_puts_raw(text);
    return character;
}

int createos_tinygl_abs(int value)
{
    return value < 0 ? -value : value;
}

void createos_tinygl_exit(int status)
{
    (void)status;
    serial_puts_raw("TinyGL fatal error\n");
    for (;;) __asm__ volatile("hlt");
}

__asm__(".section .note.GNU-stack,\"\",@progbits");
