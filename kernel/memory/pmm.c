// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "memory/pmm.h"
#include <stdint.h>
#include <stddef.h>

extern uint8_t kernel_end;

static uint8_t  frame_bitmap[BITMAP_SIZE];
static uint64_t total_frames = 0;
static uint64_t frame_bitmap_start_frame = 0;
static uint64_t allocation_hint_frame = 0;
uint64_t pmm_total_memory(void) {
    return total_frames * FRAME_SIZE;
}

uint64_t pmm_free_memory(void) {
    return pmm_get_free_frames() * FRAME_SIZE;
}

void pmm_init(uint32_t mem_lower, uint32_t mem_upper)
{
    pmm_init_bytes((uint64_t)(mem_lower + mem_upper) * 1024);
}

void pmm_init_bytes(uint64_t memory_bytes)
{
    for (uint64_t i = 0; i < BITMAP_SIZE; i++)
        frame_bitmap[i] = 0;

    total_frames = memory_bytes / FRAME_SIZE;
    if (total_frames > MAX_FRAMES)
        total_frames = MAX_FRAMES;

    frame_bitmap_start_frame =
        ((uint64_t)&kernel_end + FRAME_SIZE - 1) & ~(FRAME_SIZE - 1);

    uint64_t start_frame = frame_bitmap_start_frame / FRAME_SIZE;
    allocation_hint_frame = start_frame < total_frames
        ? start_frame : total_frames;

    for (uint64_t i = 0; i < start_frame; i++)
        frame_bitmap[i / 8] |= (1 << (i % 8));

    for (uint64_t i = start_frame; i < total_frames; i++)
        frame_bitmap[i / 8] &= ~(1 << (i % 8));
}

void pmm_init_mmap(const void* entries, uint32_t length, uint32_t entry_size)
{
    const uint8_t* data = (const uint8_t*)entries;
    const uint64_t mapped_limit = 0x40000000ULL;
    uint64_t highest = 0;

    if (!data || entry_size < 24) {
        pmm_init_bytes(0);
        return;
    }

    for (uint32_t off = 0; off + entry_size <= length; off += entry_size) {
        const uint8_t* entry = data + off;
        uint64_t address = *(const uint64_t*)entry;
        uint64_t size = *(const uint64_t*)(entry + 8);
        uint32_t type = *(const uint32_t*)(entry + 16);
        uint64_t end = address + size;
        if (type == 1 && end > highest) highest = end;
    }

    if (highest > mapped_limit) highest = mapped_limit;
    pmm_init_bytes(highest);

    /* pmm_init_bytes starts free after the kernel; reserve all non-RAM holes. */
    for (uint64_t i = 0; i < total_frames; i++)
        frame_bitmap[i / 8] |= (uint8_t)(1U << (i % 8));

    for (uint32_t off = 0; off + entry_size <= length; off += entry_size) {
        const uint8_t* entry = data + off;
        uint64_t address = *(const uint64_t*)entry;
        uint64_t size = *(const uint64_t*)(entry + 8);
        uint32_t type = *(const uint32_t*)(entry + 16);
        uint64_t start = (address + FRAME_SIZE - 1) / FRAME_SIZE;
        uint64_t end = (address + size) / FRAME_SIZE;
        if (type != 1) continue;
        if (start >= total_frames) continue;
        if (end > total_frames) end = total_frames;
        for (uint64_t frame = start; frame < end; frame++)
            frame_bitmap[frame / 8] &= (uint8_t)~(1U << (frame % 8));
    }

    uint64_t kernel_frame = frame_bitmap_start_frame / FRAME_SIZE;
    for (uint64_t frame = 0; frame < kernel_frame && frame < total_frames; frame++)
        frame_bitmap[frame / 8] |= (uint8_t)(1U << (frame % 8));
    allocation_hint_frame = kernel_frame < total_frames
        ? kernel_frame : total_frames;
}

void* pmm_alloc_frame(void)
{
    uint64_t first_free_frame = frame_bitmap_start_frame / FRAME_SIZE;
    uint64_t start_frame = allocation_hint_frame;

    if (start_frame < first_free_frame || start_frame >= total_frames)
        start_frame = first_free_frame;

    for (int pass = 0; pass < 2; pass++) {
        uint64_t first = pass == 0 ? start_frame : first_free_frame;
        uint64_t limit = pass == 0 ? total_frames : start_frame;
        for (uint64_t frame = first; frame < limit; frame++) {
            if (frame_bitmap[frame / 8] & (1U << (frame % 8)))
                continue;
            frame_bitmap[frame / 8] |=
                (uint8_t)(1U << (frame % 8));
            allocation_hint_frame = frame + 1;
            if (allocation_hint_frame >= total_frames)
                allocation_hint_frame = first_free_frame;
            return (void*)(frame * FRAME_SIZE);
        }
    }
    return NULL;
}

void pmm_reserve_range(uint64_t address, uint64_t size)
{
    uint64_t first;
    uint64_t last;
    if (!size || address >= total_frames * FRAME_SIZE) return;
    first = address / FRAME_SIZE;
    last = (address + size + FRAME_SIZE - 1) / FRAME_SIZE;
    if (last > total_frames) last = total_frames;
    for (uint64_t frame = first; frame < last; frame++)
        frame_bitmap[frame / 8] |= (uint8_t)(1U << (frame % 8));
}

void pmm_free_frame(void* addr)
{
    uint64_t address = (uint64_t)addr;
    if (address == 0 || (address & (FRAME_SIZE - 1)) != 0) return;

    uint64_t frame = address / FRAME_SIZE;
    uint64_t first_free_frame = frame_bitmap_start_frame / FRAME_SIZE;
    if (frame < first_free_frame || frame >= total_frames) return;

    frame_bitmap[frame / 8] &= ~(1 << (frame % 8));
    if (frame < allocation_hint_frame)
        allocation_hint_frame = frame;
}

uint64_t pmm_get_total_frames(void)
{
    return total_frames;
}

uint64_t pmm_get_free_frames(void)
{
    uint64_t free_frames = 0;

    for (uint64_t i = 0; i < total_frames; i++) {
        if (!(frame_bitmap[i / 8] & (1 << (i % 8)))) {
            free_frames++;
        }
    }

    return free_frames;
}
