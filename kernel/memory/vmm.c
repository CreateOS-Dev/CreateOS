// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "memory/vmm.h"
#include "memory/pmm.h"
#include "arch/x86/cpu.h"
#include "lib/string.h"
#include <stdint.h>
#include <stddef.h>

extern uint64_t pml4_table[512];

static int nx_enabled;

static uint64_t* vmm_alloc_table(void)
{
    uint64_t* table = (uint64_t*)pmm_alloc_frame();
    if (!table) return NULL;
    for (int i = 0; i < 512; i++) table[i] = 0;
    return table;
}

static inline uint64_t* vmm_get_next_level(uint64_t* table, uint64_t index, uint64_t flags)
{
    if (!(table[index] & PAGE_PRESENT))
    {
        void* frame = pmm_alloc_frame();
        if (!frame) return NULL;

        uint64_t addr = (uint64_t)frame;
        table[index] = addr | PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER);

        uint64_t* next = (uint64_t*)addr;
        for (int i = 0; i < 512; i++) next[i] = 0;

        return next;
    }

    if (flags & PAGE_USER) {
        table[index] |= PAGE_USER;
    }

    return (uint64_t*)(table[index] & PAGE_FRAME_MASK);
}

void vmm_init(void)
{
    uint64_t efer;

    if (!cpu_has_msr() || !cpu_has_nx()) return;

    efer = cpu_read_msr(0xC0000080U);
    efer |= 1ULL << 11;
    cpu_write_msr(0xC0000080U, efer);
    nx_enabled = 1;
}

int vmm_nx_enabled(void)
{
    return nx_enabled;
}

void* vmm_map_frame(uint64_t phys_addr, uint64_t flags)
{
    uint64_t virt = phys_addr;

    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    uint64_t* pdpt = vmm_get_next_level(pml4_table, pml4_i, flags);
    if (!pdpt) return NULL;

    uint64_t* pd = vmm_get_next_level(pdpt, pdpt_i, flags);
    if (!pd) return NULL;

    if ((pd[pd_i] & PAGE_PRESENT) && (pd[pd_i] & PAGE_LARGE)) {
        if ((phys_addr & 0x1FFFFFULL) == (virt & 0x1FFFFFULL))
            return (void*)virt;
        return NULL;
    }

    uint64_t* pt = vmm_get_next_level(pd, pd_i, flags);
    if (!pt) return NULL;

    pt[pt_i] = (phys_addr & PAGE_FRAME_MASK) | flags | PAGE_PRESENT;

    return (void*)virt;
}

static int vmm_split_large_page(uint64_t* directory, uint64_t index)
{
    uint64_t large_entry = directory[index];
    uint64_t* table;
    uint64_t base;
    uint64_t flags;

    if (!(large_entry & PAGE_PRESENT) || !(large_entry & PAGE_LARGE))
        return 0;
    table = vmm_alloc_table();
    if (!table) return 0;
    base = large_entry & VMM_2M_FRAME_MASK;
    flags = large_entry & (PAGE_PRESENT | PAGE_RW | PAGE_USER |
                           PAGE_PWT | PAGE_PCD | PAGE_NX);
    if (large_entry & (1ULL << 12))
        flags |= PAGE_LARGE;
    for (uint64_t i = 0; i < 512; i++)
        table[i] = (base + (i << 12)) | flags;
    directory[index] = (uint64_t)(uintptr_t)table |
                       PAGE_PRESENT | PAGE_RW |
                       (large_entry & PAGE_USER);
    cpu_invalidate_page((void*)(uintptr_t)(base));
    return 1;
}

void* vmm_map_mmio_range(uint64_t phys_addr, uint64_t size)
{
    uint64_t start;
    uint64_t end;
    const uint64_t identity_map_limit = 0x0000800000000000ULL;

    if (!size || phys_addr >= identity_map_limit ||
        size > identity_map_limit - phys_addr)
        return NULL;
    start = phys_addr & ~0xFFFULL;
    end = (phys_addr + size + 0xFFFULL) & ~0xFFFULL;

    for (uint64_t address = start; address < end; address += 0x1000) {
        uint64_t pml4_index = (address >> 39) & 0x1FF;
        uint64_t pdpt_index = (address >> 30) & 0x1FF;
        uint64_t pd_index = (address >> 21) & 0x1FF;
        uint64_t pt_index = (address >> 12) & 0x1FF;
        uint64_t* pdpt;
        uint64_t* pd;
        uint64_t* pt;

        pdpt = vmm_get_next_level(pml4_table, pml4_index, PAGE_RW);
        if (!pdpt) return NULL;
        if (pdpt[pdpt_index] & PAGE_LARGE)
            return NULL;
        pd = vmm_get_next_level(pdpt, pdpt_index, PAGE_RW);
        if (!pd) return NULL;
        if ((pd[pd_index] & PAGE_LARGE) &&
            !vmm_split_large_page(pd, pd_index))
            return NULL;
        pt = vmm_get_next_level(pd, pd_index, PAGE_RW);
        if (!pt) return NULL;
        if (!(pt[pt_index] & PAGE_PRESENT)) {
            pt[pt_index] = address | PAGE_PRESENT | PAGE_RW |
                           PAGE_PCD | PAGE_PWT;
        } else if ((pt[pt_index] & PAGE_FRAME_MASK) != address) {
            return NULL;
        } else {
            pt[pt_index] |= PAGE_PCD | PAGE_PWT;
        }
        cpu_invalidate_page((void*)(uintptr_t)address);
    }
    return (void*)(uintptr_t)phys_addr;
}

void vmm_unmap_frame(void* virt_addr)
{
    uint64_t virt = (uint64_t)virt_addr;
    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    if (!(pml4_table[pml4_i] & PAGE_PRESENT)) return;
    uint64_t* pdpt = (uint64_t*)(pml4_table[pml4_i] & PAGE_FRAME_MASK);
    if (!(pdpt[pdpt_i] & PAGE_PRESENT)) return;
    uint64_t* pd = (uint64_t*)(pdpt[pdpt_i] & PAGE_FRAME_MASK);
    if (!(pd[pd_i] & PAGE_PRESENT) || (pd[pd_i] & PAGE_LARGE)) return;
    uint64_t* pt = (uint64_t*)(pd[pd_i] & PAGE_FRAME_MASK);
    pt[pt_i] = 0;
    cpu_invalidate_page(virt_addr);
}

void* vmm_map_user_frame(uint64_t phys_addr)
{
    return vmm_map_frame(phys_addr, PAGE_USER | PAGE_RW);
}

void vmm_unmap_user_frame(void* virt_addr)
{
    vmm_unmap_frame(virt_addr);
}

void* vmm_get_phys_addr(void* virt_addr)
{
    uint64_t virt = (uint64_t)virt_addr;
    uint64_t pml4_i = (virt >> 39) & 0x1FF;
    uint64_t pdpt_i = (virt >> 30) & 0x1FF;
    uint64_t pd_i   = (virt >> 21) & 0x1FF;
    uint64_t pt_i   = (virt >> 12) & 0x1FF;

    if (!(pml4_table[pml4_i] & PAGE_PRESENT)) return NULL;
    uint64_t* pdpt = (uint64_t*)(pml4_table[pml4_i] & PAGE_FRAME_MASK);
    if (!(pdpt[pdpt_i] & PAGE_PRESENT)) return NULL;
    if (!(pdpt[pdpt_i] & PAGE_PRESENT)) return NULL;
    if (pdpt[pdpt_i] & PAGE_LARGE) {
        return (void*)((pdpt[pdpt_i] & VMM_1G_FRAME_MASK) |
                       (virt & 0x3FFFFFFFULL));
    }

    uint64_t* pd = (uint64_t*)(pdpt[pdpt_i] & PAGE_FRAME_MASK);
    if (!(pd[pd_i] & PAGE_PRESENT)) return NULL;
    if (pd[pd_i] & PAGE_LARGE) {
        return (void*)((pd[pd_i] & VMM_2M_FRAME_MASK) |
                       (virt & 0x1FFFFFULL));
    }

    uint64_t* pt = (uint64_t*)(pd[pd_i] & PAGE_FRAME_MASK);
    if (!(pt[pt_i] & PAGE_PRESENT)) return NULL;

    return (void*)((pt[pt_i] & PAGE_FRAME_MASK) | (virt & 0xFFF));
}

int vmm_address_space_create(vmm_address_space_t* space)
{
    uint64_t* kernel_pdpt;
    uint64_t* user_pdpt;

    if (!space || space->pml4) return 0;
    space->pml4 = vmm_alloc_table();
    if (!space->pml4) return 0;

    for (int i = 256; i < 512; i++) {
        if (!(pml4_table[i] & PAGE_PRESENT)) continue;
        space->pml4[i] = pml4_table[i];
    }

    kernel_pdpt = (uint64_t*)(pml4_table[0] & PAGE_FRAME_MASK);
    user_pdpt = vmm_alloc_table();
    if (!user_pdpt) goto fail;
    space->pml4[0] = (uint64_t)user_pdpt |
        ((pml4_table[0] & 0xFFFULL) | PAGE_USER);

    for (int i = 0; i < 512; i++) {
        uint64_t pdpt_entry = kernel_pdpt[i];
        if (!(pdpt_entry & PAGE_PRESENT)) continue;
        /* PDPT entry 1 (1–2 GiB) is reserved for ring-3 mappings. */
        if (i == 1)
            continue;

        if (pdpt_entry & PAGE_LARGE) {
            user_pdpt[i] = pdpt_entry & ~PAGE_USER;
            continue;
        }

        uint64_t* kernel_pd = (uint64_t*)(pdpt_entry & PAGE_FRAME_MASK);
        uint64_t* user_pd = vmm_alloc_table();
        if (!user_pd) goto fail;
        user_pdpt[i] = (uint64_t)user_pd |
            ((pdpt_entry & 0xFFFULL) | PAGE_USER);

        for (int j = 0; j < 512; j++) {
            uint64_t pd_entry = kernel_pd[j];
            if (!(pd_entry & PAGE_PRESENT)) continue;
            if (pd_entry & PAGE_LARGE) {
                uint64_t* user_pt = vmm_alloc_table();
                uint64_t base = pd_entry & VMM_2M_FRAME_MASK;
                uint64_t flags = pd_entry & (0xFFFULL | PAGE_NX) &
                    ~(PAGE_LARGE | PAGE_USER);
                if (!user_pt) goto fail;
                for (int k = 0; k < 512; k++)
                    user_pt[k] = (base + (uint64_t)k * 0x1000) |
                        flags | PAGE_PRESENT;
                user_pd[j] = (uint64_t)user_pt | flags;
                continue;
            }

            uint64_t* kernel_pt = (uint64_t*)(pd_entry & PAGE_FRAME_MASK);
            uint64_t* user_pt = vmm_alloc_table();
            if (!user_pt) goto fail;
            for (int k = 0; k < 512; k++)
                user_pt[k] = kernel_pt[k] & ~PAGE_USER;
            user_pd[j] = (uint64_t)user_pt | (pd_entry & 0xFFFULL);
        }
    }

    return 1;

fail:
    vmm_address_space_destroy(space);
    return 0;
}

int vmm_address_space_clone(const vmm_address_space_t* source,
                            vmm_address_space_t* clone)
{
    uint64_t pml4_entry;
    uint64_t* pdpt;
    uint64_t pdpt_entry;
    uint64_t* pd;

    if (!source || !source->pml4 || !clone || clone->pml4 ||
        !vmm_address_space_create(clone))
        return 0;

    pml4_entry = source->pml4[(VMM_USER_ADDRESS_BASE >> 39) & 0x1FF];
    if (!(pml4_entry & PAGE_PRESENT)) return 1;
    pdpt = (uint64_t*)(pml4_entry & PAGE_FRAME_MASK);
    pdpt_entry = pdpt[(VMM_USER_ADDRESS_BASE >> 30) & 0x1FF];
    if (!(pdpt_entry & PAGE_PRESENT)) return 1;
    if (pdpt_entry & PAGE_LARGE) goto fail;
    pd = (uint64_t*)(pdpt_entry & PAGE_FRAME_MASK);

    for (uint32_t directory_index = 0; directory_index < 512;
         directory_index++) {
        uint64_t directory_entry = pd[directory_index];
        uint64_t* page_table;
        uint64_t large_base = 0;
        uint64_t large_flags = 0;

        if (!(directory_entry & PAGE_PRESENT)) continue;
        if (directory_entry & PAGE_LARGE) {
            large_base = directory_entry & VMM_2M_FRAME_MASK;
            large_flags = directory_entry & (PAGE_RW | PAGE_NX);
        } else {
            page_table =
                (uint64_t*)(directory_entry & PAGE_FRAME_MASK);
        }

        for (uint32_t table_index = 0; table_index < 512; table_index++) {
            uint64_t page_entry;
            uint64_t source_frame;
            uint64_t flags;
            uint64_t address =
                VMM_USER_ADDRESS_BASE +
                ((uint64_t)directory_index * 512 + table_index) * 0x1000;
            uint8_t* frame;

            if (directory_entry & PAGE_LARGE) {
                page_entry = directory_entry;
                source_frame = large_base + (uint64_t)table_index * 0x1000;
                flags = large_flags;
            } else {
                page_entry = page_table[table_index];
                if (!(page_entry & PAGE_PRESENT) ||
                    !(page_entry & PAGE_USER))
                    continue;
                source_frame = page_entry & PAGE_FRAME_MASK;
                flags = page_entry & (PAGE_RW | PAGE_NX);
            }

            if (!(page_entry & PAGE_USER)) continue;
            frame = (uint8_t*)pmm_alloc_frame();
            if (!frame) goto fail;
            memcpy(frame, (const void*)(uintptr_t)source_frame, 0x1000);
            if (!vmm_address_space_map(clone, address,
                                       (uint64_t)(uintptr_t)frame, flags)) {
                pmm_free_frame(frame);
                goto fail;
            }
        }
    }
    return 1;

fail:
    vmm_address_space_destroy(clone);
    return 0;
}

static void vmm_space_destroy_table(uint64_t* table, int level)
{
    for (int i = 0; i < 512; i++) {
        uint64_t entry = table[i];
        uint64_t* child;
        if (!(entry & PAGE_PRESENT)) continue;

        if (level == 1) {
            if (entry & PAGE_USER)
                pmm_free_frame((void*)(entry & PAGE_FRAME_MASK));
            continue;
        }
        if (entry & PAGE_LARGE) continue;

        child = (uint64_t*)(entry & PAGE_FRAME_MASK);
        vmm_space_destroy_table(child, level - 1);
        pmm_free_frame(child);
    }
}

void vmm_address_space_destroy(vmm_address_space_t* space)
{
    if (!space || !space->pml4) return;

    for (int i = 0; i < 256; i++) {
        uint64_t entry = space->pml4[i];
        uint64_t* pdpt;
        if (!(entry & PAGE_PRESENT) || (entry & PAGE_LARGE)) continue;
        pdpt = (uint64_t*)(entry & PAGE_FRAME_MASK);
        vmm_space_destroy_table(pdpt, 3);
        pmm_free_frame(pdpt);
    }

    pmm_free_frame(space->pml4);
    space->pml4 = NULL;
}

static uint64_t* vmm_space_next(uint64_t* table, uint64_t index,
                                uint64_t large_page_size)
{
    if ((table[index] & PAGE_PRESENT) && (table[index] & PAGE_LARGE)) {
        uint64_t* split;
        uint64_t base;
        uint64_t flags;
        uint64_t child_page_size;
        if (large_page_size != 0x40000000ULL &&
            large_page_size != 0x200000ULL)
            return NULL;
        split = vmm_alloc_table();
        if (!split) return NULL;
        if (large_page_size == 0x40000000ULL) {
            base = table[index] & VMM_1G_FRAME_MASK;
            child_page_size = 0x200000ULL;
            flags = table[index] & (0xFFFULL | PAGE_NX) &
                ~(PAGE_LARGE | PAGE_USER);
            for (int i = 0; i < 512; i++)
                split[i] = (base + (uint64_t)i * child_page_size) |
                    flags | PAGE_PRESENT | PAGE_LARGE;
        } else {
            base = table[index] & VMM_2M_FRAME_MASK;
            child_page_size = 0x1000ULL;
            flags = table[index] & (0xFFFULL | PAGE_NX) &
                ~(PAGE_LARGE | PAGE_USER);
            for (int i = 0; i < 512; i++)
                split[i] = (base + (uint64_t)i * child_page_size) |
                    flags | PAGE_PRESENT;
        }
        table[index] = (uint64_t)split | PAGE_PRESENT | PAGE_RW;
    }
    if (!(table[index] & PAGE_PRESENT)) {
        uint64_t* child = vmm_alloc_table();
        if (!child) return NULL;
        table[index] = (uint64_t)child | PAGE_PRESENT | PAGE_RW;
    }
    if (table[index] & PAGE_LARGE) return NULL;
    return (uint64_t*)(table[index] & PAGE_FRAME_MASK);
}

static int vmm_table_empty(const uint64_t* table)
{
    for (int i = 0; i < 512; i++)
        if (table[i] & PAGE_PRESENT) return 0;
    return 1;
}

int vmm_address_space_map(vmm_address_space_t* space, uint64_t virt_addr,
                          uint64_t phys_addr, uint64_t flags)
{
    uint64_t pml4_index = (virt_addr >> 39) & 0x1FF;
    uint64_t pdpt_index = (virt_addr >> 30) & 0x1FF;
    uint64_t pd_index = (virt_addr >> 21) & 0x1FF;
    uint64_t* pdpt;
    uint64_t* pd;
    uint64_t* pt;
    uint64_t* pte;

    if (!space || !space->pml4 ||
        virt_addr < VMM_USER_ADDRESS_BASE ||
        virt_addr >= VMM_USER_ADDRESS_LIMIT ||
        (virt_addr & 0xFFF) || (phys_addr & 0xFFF) ||
        (phys_addr & ~PAGE_FRAME_MASK) ||
        (flags & ~(PAGE_RW | PAGE_USER | PAGE_NX)) ||
        ((flags & PAGE_NX) && !nx_enabled))
        return 0;

    pdpt = vmm_space_next(space->pml4, pml4_index, 0);
    if (!pdpt) return 0;
    space->pml4[pml4_index] |= PAGE_USER | PAGE_RW;

    pd = vmm_space_next(pdpt, pdpt_index, 0x40000000ULL);
    if (!pd) return 0;
    pdpt[pdpt_index] |= PAGE_USER | PAGE_RW;

    pt = vmm_space_next(pd, pd_index, 0x200000ULL);
    if (!pt) return 0;
    pd[pd_index] |= PAGE_USER | PAGE_RW;

    pte = &pt[(virt_addr >> 12) & 0x1FF];
    if (*pte & PAGE_PRESENT) return 0;
    *pte = (phys_addr & PAGE_FRAME_MASK) |
        flags | PAGE_PRESENT | PAGE_USER;

    __asm__ volatile("invlpg (%0)" : : "r"((void*)virt_addr) : "memory");
    return 1;
}

int vmm_address_space_unmap(vmm_address_space_t* space, uint64_t virt_addr,
                            uint64_t* phys_addr)
{
    uint64_t* table;
    uint64_t entry;
    uint64_t pml4_index;
    uint64_t pdpt_index;
    uint64_t pd_index;
    uint64_t* pdpt;
    uint64_t* pd;
    uint64_t* pt;

    if (phys_addr) *phys_addr = 0;
    if (!space || !space->pml4 ||
        virt_addr < VMM_USER_ADDRESS_BASE ||
        virt_addr >= VMM_USER_ADDRESS_LIMIT ||
        (virt_addr & 0xFFF))
        return 0;

    pml4_index = (virt_addr >> 39) & 0x1FF;
    pdpt_index = (virt_addr >> 30) & 0x1FF;
    pd_index = (virt_addr >> 21) & 0x1FF;
    entry = space->pml4[pml4_index];
    if (!(entry & PAGE_PRESENT) || (entry & PAGE_LARGE)) return 0;
    pdpt = (uint64_t*)(entry & PAGE_FRAME_MASK);
    entry = pdpt[pdpt_index];
    if (!(entry & PAGE_PRESENT) || (entry & PAGE_LARGE)) return 0;
    pd = (uint64_t*)(entry & PAGE_FRAME_MASK);
    entry = pd[pd_index];
    if (!(entry & PAGE_PRESENT) || (entry & PAGE_LARGE)) return 0;
    pt = (uint64_t*)(entry & PAGE_FRAME_MASK);
    table = &pt[(virt_addr >> 12) & 0x1FF];
    entry = *table;
    if (!(entry & PAGE_PRESENT) || !(entry & PAGE_USER)) return 0;

    *table = 0;
    if (phys_addr) *phys_addr = entry & PAGE_FRAME_MASK;
    __asm__ volatile("invlpg (%0)" : : "r"((void*)virt_addr) : "memory");

    if (vmm_table_empty(pt)) {
        pd[pd_index] = 0;
        pmm_free_frame(pt);
        if (vmm_table_empty(pd)) {
            pdpt[pdpt_index] = 0;
            pmm_free_frame(pd);
            if (vmm_table_empty(pdpt)) {
                space->pml4[pml4_index] = 0;
                pmm_free_frame(pdpt);
            }
        }
    }
    return 1;
}

uint64_t vmm_address_space_page_flags(const vmm_address_space_t* space,
                                      uint64_t virt_addr)
{
    uint64_t* table;
    uint64_t entry;
    if (!space || !space->pml4) return 0;
    entry = space->pml4[(virt_addr >> 39) & 0x1FF];
    if (!(entry & PAGE_PRESENT)) return 0;
    table = (uint64_t*)(entry & PAGE_FRAME_MASK);
    entry = table[(virt_addr >> 30) & 0x1FF];
    if (!(entry & PAGE_PRESENT)) return 0;
    if (entry & PAGE_LARGE)
        return entry & (0xFFFULL | PAGE_NX);
    table = (uint64_t*)(entry & PAGE_FRAME_MASK);
    entry = table[(virt_addr >> 21) & 0x1FF];
    if (!(entry & PAGE_PRESENT)) return 0;
    if (entry & PAGE_LARGE)
        return entry & (0xFFFULL | PAGE_NX);
    table = (uint64_t*)(entry & PAGE_FRAME_MASK);
    entry = table[(virt_addr >> 12) & 0x1FF];
    return entry & PAGE_PRESENT ?
        entry & (0xFFFULL | PAGE_NX) : 0;
}

uint64_t vmm_address_space_page_frame(const vmm_address_space_t* space,
                                      uint64_t virt_addr)
{
    uint64_t* table;
    uint64_t entry;
    if (!space || !space->pml4) return 0;
    entry = space->pml4[(virt_addr >> 39) & 0x1FF];
    if (!(entry & PAGE_PRESENT)) return 0;
    table = (uint64_t*)(entry & PAGE_FRAME_MASK);
    entry = table[(virt_addr >> 30) & 0x1FF];
    if (!(entry & PAGE_PRESENT)) return 0;
    if (entry & PAGE_LARGE)
        return (entry & VMM_1G_FRAME_MASK) |
               (virt_addr & 0x3FFFFFFFULL);
    table = (uint64_t*)(entry & PAGE_FRAME_MASK);
    entry = table[(virt_addr >> 21) & 0x1FF];
    if (!(entry & PAGE_PRESENT)) return 0;
    if (entry & PAGE_LARGE)
        return (entry & VMM_2M_FRAME_MASK) |
               (virt_addr & 0x1FFFFFULL);
    table = (uint64_t*)(entry & PAGE_FRAME_MASK);
    entry = table[(virt_addr >> 12) & 0x1FF];
    if (!(entry & PAGE_PRESENT)) return 0;
    return entry & PAGE_FRAME_MASK;
}

uint64_t vmm_address_space_parent_flags(const vmm_address_space_t* space,
                                        uint64_t virt_addr)
{
    uint64_t entry;
    uint64_t* table;
    uint64_t result = 0;
    if (!space || !space->pml4) return 0;
    entry = space->pml4[(virt_addr >> 39) & 0x1FF];
    if (!(entry & PAGE_PRESENT)) return 0;
    if (entry & PAGE_USER) result |= 0x100;
    table = (uint64_t*)(entry & PAGE_FRAME_MASK);
    entry = table[(virt_addr >> 30) & 0x1FF];
    if (!(entry & PAGE_PRESENT)) return result;
    if (entry & PAGE_USER) result |= 0x200;
    table = (uint64_t*)(entry & PAGE_FRAME_MASK);
    entry = table[(virt_addr >> 21) & 0x1FF];
    if (entry & PAGE_USER) result |= 0x400;
    return result;
}

int vmm_user_range_accessible(uint64_t address, uint64_t size,
                              int require_write)
{
    uint64_t end;
    uint64_t cr3;
    uint64_t* pml4;
    uint64_t page;

    if (address < VMM_USER_ADDRESS_BASE ||
        address >= VMM_USER_ADDRESS_LIMIT ||
        size > VMM_USER_ADDRESS_LIMIT - address)
        return 0;
    if (!size) return 1;
    end = address + size;

    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    pml4 = (uint64_t*)(uintptr_t)(cr3 & PAGE_FRAME_MASK);
    page = address & ~0xFFFULL;
    while (page < end) {
        uint64_t entry = pml4[(page >> 39) & 0x1FF];
        uint64_t permissions = PAGE_PRESENT | PAGE_USER;
        uint64_t* table;

        if (require_write) permissions |= PAGE_RW;
        if ((entry & permissions) != permissions) return 0;
        table = (uint64_t*)(uintptr_t)(entry & PAGE_FRAME_MASK);
        entry = table[(page >> 30) & 0x1FF];
        if ((entry & permissions) != permissions) return 0;
        if (entry & PAGE_LARGE) {
            page += 0x40000000ULL - (page & 0x3FFFFFFFULL);
            continue;
        }

        table = (uint64_t*)(uintptr_t)(entry & PAGE_FRAME_MASK);
        entry = table[(page >> 21) & 0x1FF];
        if ((entry & permissions) != permissions) return 0;
        if (entry & PAGE_LARGE) {
            page += 0x200000ULL - (page & 0x1FFFFFULL);
            continue;
        }

        table = (uint64_t*)(uintptr_t)(entry & PAGE_FRAME_MASK);
        entry = table[(page >> 12) & 0x1FF];
        if ((entry & permissions) != permissions) return 0;
        page += 0x1000;
    }
    return 1;
}

void vmm_address_space_activate(const vmm_address_space_t* space)
{
    if (!space || !space->pml4) return;
    
    // Obtenir l'adresse physique du pointeur space->pml4 à travers la table noyau pml4_table
    uint64_t phys_pml4 = (uint64_t)vmm_get_phys_addr((void*)space->pml4);
    if (!phys_pml4) phys_pml4 = (uint64_t)space->pml4;

    __asm__ volatile("mov %0, %%cr3" :: "r"(phys_pml4) : "memory");
}