// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define FRAME_SIZE 4096
#define MAX_FRAMES   (1024 * 1024)
#define BITMAP_SIZE  (MAX_FRAMES / 8)

void     pmm_init(uint32_t mem_lower, uint32_t mem_upper);
void     pmm_init_bytes(uint64_t memory_bytes);
void     pmm_init_mmap(const void* entries, uint32_t length, uint32_t entry_size);
void     pmm_reserve_range(uint64_t address, uint64_t size);
void*    pmm_alloc_frame(void);
void     pmm_free_frame(void* addr);
uint64_t pmm_get_total_frames(void);
uint64_t pmm_get_free_frames(void);
uint64_t pmm_total_memory(void);
uint64_t pmm_free_memory(void);

#endif
