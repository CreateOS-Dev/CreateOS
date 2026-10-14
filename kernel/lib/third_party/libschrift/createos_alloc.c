#include "createos_alloc.h"
#include <stdint.h>
#include "lib/string.h"

#define SCHRIFT_HEAP_SIZE (16U * 1024U * 1024U)
#define SCHRIFT_HEAP_ALIGNMENT 16U

typedef struct schrift_heap_block {
    size_t size;
    struct schrift_heap_block* next;
    int free;
    uint8_t reserved[8];
} schrift_heap_block_t;

static union {
    max_align_t alignment;
    uint8_t bytes[SCHRIFT_HEAP_SIZE];
} schrift_heap;
static schrift_heap_block_t* schrift_heap_first;

static size_t align_size(size_t size)
{
    return (size + SCHRIFT_HEAP_ALIGNMENT - 1) &
           ~(size_t)(SCHRIFT_HEAP_ALIGNMENT - 1);
}

void* createos_schrift_malloc(size_t size)
{
    schrift_heap_block_t* block;
    if (!size || size > SCHRIFT_HEAP_SIZE - sizeof(*block))
        return NULL;
    size = align_size(size);
    if (!schrift_heap_first) {
        schrift_heap_first = (schrift_heap_block_t*)schrift_heap.bytes;
        schrift_heap_first->size = SCHRIFT_HEAP_SIZE - sizeof(*block);
        schrift_heap_first->next = NULL;
        schrift_heap_first->free = 1;
    }
    for (block = schrift_heap_first; block; block = block->next) {
        if (!block->free || block->size < size)
            continue;
        if (block->size >= size + sizeof(*block) + SCHRIFT_HEAP_ALIGNMENT) {
            schrift_heap_block_t* remainder =
                (schrift_heap_block_t*)((uint8_t*)(block + 1) + size);
            remainder->size = block->size - size - sizeof(*block);
            remainder->next = block->next;
            remainder->free = 1;
            block->next = remainder;
            block->size = size;
        }
        block->free = 0;
        return block + 1;
    }
    return NULL;
}

void* createos_schrift_calloc(size_t count, size_t size)
{
    size_t total;
    void* memory;
    if (size && count > (size_t)-1 / size)
        return NULL;
    total = count * size;
    memory = createos_schrift_malloc(total);
    if (memory)
        memset(memory, 0, total);
    return memory;
}

void createos_schrift_free(void* ptr)
{
    schrift_heap_block_t* block;
    if (!ptr || (uint8_t*)ptr < schrift_heap.bytes ||
        (uint8_t*)ptr >= schrift_heap.bytes + sizeof(schrift_heap.bytes))
        return;
    block = (schrift_heap_block_t*)ptr - 1;
    block->free = 1;
    for (block = schrift_heap_first; block && block->next; ) {
        if (block->free && block->next->free) {
            block->size += sizeof(*block) + block->next->size;
            block->next = block->next->next;
        } else {
            block = block->next;
        }
    }
}

void* createos_schrift_realloc(void* ptr, size_t size)
{
    schrift_heap_block_t* block;
    void* replacement;
    if (!ptr)
        return createos_schrift_malloc(size);
    if (!size) {
        createos_schrift_free(ptr);
        return NULL;
    }
    block = (schrift_heap_block_t*)ptr - 1;
    if (block->size >= size)
        return ptr;
    replacement = createos_schrift_malloc(size);
    if (!replacement)
        return NULL;
    memcpy(replacement, ptr, block->size);
    createos_schrift_free(ptr);
    return replacement;
}

__asm__(".section .note.GNU-stack,\"\",@progbits");
