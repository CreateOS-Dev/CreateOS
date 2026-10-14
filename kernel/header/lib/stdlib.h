// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

int atoi(const char* s);
long strtol(const char* s, char** end, int base);

void* malloc(size_t size);
void free(void* ptr);

#endif
