#ifndef CREATEOS_SCHRIFT_ALLOC_H
#define CREATEOS_SCHRIFT_ALLOC_H

#include <stddef.h>

void* createos_schrift_malloc(size_t size);
void* createos_schrift_calloc(size_t count, size_t size);
void* createos_schrift_realloc(void* ptr, size_t size);
void createos_schrift_free(void* ptr);

#endif
