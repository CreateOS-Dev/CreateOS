#ifndef CREATEOS_TINYGL_STDLIB_H
#define CREATEOS_TINYGL_STDLIB_H

#include <stddef.h>
#include "createos_alloc.h"

#define malloc createos_schrift_malloc
#define calloc createos_schrift_calloc
#define realloc createos_schrift_realloc
#define free createos_schrift_free
#define abs createos_tinygl_abs
#define exit createos_tinygl_exit

int createos_tinygl_abs(int value);
void createos_tinygl_exit(int status) __attribute__((noreturn));

#endif
