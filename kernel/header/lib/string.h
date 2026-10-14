// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef STRING_H
#define STRING_H

#include <stdint.h>

void* memcpy(void* dst, const void* src, uint64_t n);
void* memmove(void* dst, const void* src, uint64_t n);
void* memset(void* dst, int value, uint64_t n);

int memcmp(const void* ptr1, const void* ptr2, uint64_t num);

int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, uint64_t n);

char* strcpy(char* dst, const char* src);
uint64_t strlen(const char* s);

char* strncpy(char* dst, const char* src, uint64_t n);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strcat(char* dst, const char* src);
int atoi(const char* s);
int sprintf(char* buf, const char* fmt, ...);

#endif
