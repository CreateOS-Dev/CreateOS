// ============================================================
// CreateOS 0.0.1
// ============================================================

#include <stdint.h>
#include <stdarg.h>
#include "lib/string.h"

void* memcpy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void* memmove(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;

    if (d < s) {
        for (uint64_t i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (uint64_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }

    return dst;
}

void* memset(void* dst, int value, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++)
        d[i] = (uint8_t)value;
    return dst;
}

int strcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int memcmp(const void* ptr1, const void* ptr2, uint64_t num) {
    const unsigned char* p1 = ptr1;
    const unsigned char* p2 = ptr2;
    for (uint64_t i = 0; i < num; i++) {
        if (p1[i] != p2[i])
            return p1[i] < p2[i] ? -1 : 1;
    }
    return 0;
}

int strncmp(const char* a, const char* b, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i] || !b[i])
            return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++));
    return dst;
}

uint64_t strlen(const char* s) {
    uint64_t i = 0;
    while (s[i]) i++;
    return i;
}

char* strncpy(char* dst, const char* src, uint64_t n) {
    uint64_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c)
            return (char*)s;
        s++;
    }
    if (c == 0)
        return (char*)s;
    return 0;
}

char* strrchr(const char* s, int c) {
    const char* last = 0;
    while (*s) {
        if (*s == (char)c)
            last = s;
        s++;
    }
    if (c == 0)
        return (char*)s;
    return (char*)last;
}

char* strcat(char* dst, const char* src) {
    char* r = dst;
    while (*dst) dst++;
    while ((*dst++ = *src++));
    return r;
}

int atoi(const char* s) {
    int sign = 1;
    int result = 0;

    if (*s == '-') {
        sign = -1;
        s++;
    }

    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }

    return sign * result;
}

int sprintf(char* buf, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    char* p = buf;

    while (*fmt) {
        if (*fmt != '%') {
            *p++ = *fmt++;
            continue;
        }

        fmt++;

        if (*fmt == 's') {
            const char* s = va_arg(args, const char*);
            while (*s) *p++ = *s++;
        }
        else if (*fmt == 'd') {
            int v = va_arg(args, int);
            char tmp[32];
            int i = 0;

            if (v < 0) {
                *p++ = '-';
                v = -v;
            }

            do {
                tmp[i++] = '0' + (v % 10);
                v /= 10;
            } while (v);

            while (i--) *p++ = tmp[i];
        }
        else if (*fmt == 'u') {
            unsigned v = va_arg(args, unsigned);
            char tmp[32];
            int i = 0;

            do {
                tmp[i++] = '0' + (v % 10);
                v /= 10;
            } while (v);

            while (i--) *p++ = tmp[i];
        }
        else if (*fmt == 'c') {
            *p++ = (char)va_arg(args, int);
        }

        fmt++;
    }

    *p = 0;
    va_end(args);
    return p - buf;
}
