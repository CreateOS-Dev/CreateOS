#ifndef CREATEOS_TINYGL_STDIO_H
#define CREATEOS_TINYGL_STDIO_H

#include <stdarg.h>

typedef struct createos_tinygl_file FILE;
extern FILE* stderr;
extern FILE* stdout;

int fprintf(FILE* stream, const char* format, ...);
int printf(const char* format, ...);
int fputc(int character, FILE* stream);
int vfprintf(FILE* stream, const char* format, va_list args);

#endif
