// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef VMM_H
#define VMM_H

#include <stdint.h>

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_RW       (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_PWT      (1ULL << 3)
#define PAGE_PCD      (1ULL << 4)
#define PAGE_LARGE    (1ULL << 7)
#define PAGE_NX       (1ULL << 63)
#define PAGE_FRAME_MASK 0x000FFFFFFFFFF000ULL
#define VMM_USER_ADDRESS_BASE  0x0000000040000000ULL
#define VMM_USER_ADDRESS_LIMIT 0x0000000080000000ULL
#define VMM_2M_FRAME_MASK 0x000FFFFFFFE00000ULL
#define VMM_1G_FRAME_MASK 0x000FFFFFC0000000ULL

typedef struct {
	uint64_t* pml4;
} vmm_address_space_t;

void  vmm_init(void);
int   vmm_nx_enabled(void);
void* vmm_map_frame(uint64_t phys_addr, uint64_t flags);
void* vmm_map_mmio_range(uint64_t phys_addr, uint64_t size);
void  vmm_unmap_frame(void* virt_addr);
int   vmm_address_space_create(vmm_address_space_t* space);
int   vmm_address_space_clone(const vmm_address_space_t* source,
                              vmm_address_space_t* clone);
void  vmm_address_space_destroy(vmm_address_space_t* space);
int   vmm_address_space_map(vmm_address_space_t* space, uint64_t virt_addr,
							uint64_t phys_addr, uint64_t flags);
int   vmm_address_space_unmap(vmm_address_space_t* space, uint64_t virt_addr,
                              uint64_t* phys_addr);
uint64_t vmm_address_space_page_flags(const vmm_address_space_t* space,
                                      uint64_t virt_addr);
uint64_t vmm_address_space_page_frame(const vmm_address_space_t* space,
                                      uint64_t virt_addr);
uint64_t vmm_address_space_parent_flags(const vmm_address_space_t* space,
										uint64_t virt_addr);
int   vmm_user_range_accessible(uint64_t address, uint64_t size,
                                int require_write);
void  vmm_address_space_activate(const vmm_address_space_t* space);
void* vmm_map_user_frame(uint64_t phys_addr);
void  vmm_unmap_user_frame(void* virt_addr);
void* vmm_get_phys_addr(void* virt_addr);

#endif
