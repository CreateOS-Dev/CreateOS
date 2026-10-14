// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "fs/cfs5.h"
#include "drivers/block.h"
#include "lib/string.h"
#include "memory/pmm.h"

static int cfs5_read_block_raw(uint32_t block, void* buf) {
    uint64_t lba = (uint64_t)block * (CFS5_BLOCK_SIZE / BLOCK_SECTOR_SIZE);
    if (!buf) return 0;
    for (int i = 0; i < (CFS5_BLOCK_SIZE / 512); i++) {
        if (!block_read(lba + i, (uint8_t*)buf + i * BLOCK_SECTOR_SIZE))
            return 0;
    }
    return 1;
}

static int cfs5_write_block_raw(uint32_t block, const void* buf) {
    uint64_t lba = (uint64_t)block * (CFS5_BLOCK_SIZE / BLOCK_SECTOR_SIZE);
    if (!buf) return 0;
    for (int i = 0; i < (CFS5_BLOCK_SIZE / 512); i++) {
        if (!block_write(lba + i,
                         (const uint8_t*)buf + i * BLOCK_SECTOR_SIZE))
            return 0;
    }
    return 1;
}

int cfs5_read_block(uint32_t block, void* buf) {
    cfs5_superblock_t* sb = cfs5_get_super();
    if (!sb) return 0;
    if (block >= sb->block_count) return 0;
    return cfs5_read_block_raw(block, buf);
}

int cfs5_write_block(uint32_t block, const void* buf) {
    cfs5_superblock_t* sb = cfs5_get_super();
    if (!sb) return 0;
    if (block >= sb->block_count) return 0;
    return cfs5_write_block_raw(block, buf);
}

int cfs5_alloc_block(uint32_t* out_block) {
    cfs5_superblock_t* sb = cfs5_get_super();
    if (!sb) return 0;

    uint32_t bitmap_block_start = sb->free_block_bitmap;
    uint32_t bitmap_block_count = sb->free_block_bitmap_blocks;
    uint32_t bits_per_block = 8 * CFS5_BLOCK_SIZE;
    if (!out_block || sb->data_block_start >= sb->block_count)
        return 0;
    uint32_t data_block_count = sb->block_count - sb->data_block_start;

    for (uint32_t block_offset = 0; block_offset < bitmap_block_count; block_offset++) {
        uint8_t bitmap_block[CFS5_BLOCK_SIZE];
        if (!cfs5_read_block_raw(bitmap_block_start + block_offset, bitmap_block))
            return 0;

        for (uint32_t bit = 0; bit < bits_per_block; bit++) {
            uint32_t relative_block = block_offset * bits_per_block + bit;
            if (relative_block >= data_block_count)
                return 0;
            uint32_t byte_index = bit / 8;
            uint32_t bit_in_byte = bit % 8;
            if (!(bitmap_block[byte_index] & (1 << bit_in_byte))) {
                bitmap_block[byte_index] |= (1 << bit_in_byte);
                if (!cfs5_write_block_raw(bitmap_block_start + block_offset, bitmap_block))
                    return 0;
                *out_block = sb->data_block_start + relative_block;
                return 1;
            }
        }
    }
    return 0; // No free block found
}

int cfs5_free_block(uint32_t block) {
    cfs5_superblock_t* sb = cfs5_get_super();
    if (!sb) return 0;

    uint32_t bitmap_block_start = sb->free_block_bitmap;
    uint32_t bitmap_block_count = sb->free_block_bitmap_blocks;
    uint32_t bits_per_block = 8 * CFS5_BLOCK_SIZE;

    if (block < sb->data_block_start || block >= sb->block_count)
        return 0;

    uint32_t relative_block = block - sb->data_block_start;
    uint32_t block_offset = relative_block / bits_per_block;
    if (block_offset >= bitmap_block_count)
        return 0;

    uint32_t bit_in_block = relative_block % bits_per_block;
    int byte_index = bit_in_block / 8;
    int bit_in_byte = bit_in_block % 8;

    uint8_t bitmap_block[CFS5_BLOCK_SIZE];
    if (!cfs5_read_block_raw(bitmap_block_start + block_offset, bitmap_block))
        return 0;

    if (bitmap_block[byte_index] & (1 << bit_in_byte)) {
        bitmap_block[byte_index] &= ~(1 << bit_in_byte);
        if (!cfs5_write_block_raw(bitmap_block_start + block_offset, bitmap_block))
            return 0;
        return 1;
    } else {
        return 0;
    }
}