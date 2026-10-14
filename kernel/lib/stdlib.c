// ============================================================
// CreateOS 0.0.1
// ============================================================


#include "lib/stdlib.h"
#include "memory/pmm.h"
#include <stddef.h>

void* malloc(size_t size) {
    if (size == 0 || size > FRAME_SIZE) {
        return NULL;
    }
    return pmm_alloc_frame();
}

void free(void* ptr) {
    if (ptr) {
        pmm_free_frame(ptr);
    }
}