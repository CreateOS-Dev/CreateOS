// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "fs/cfs5.h"
#include "lib/string.h"
#include "drivers/serial.h"

extern cfs5_superblock_t* cfs5_get_super();
extern int cfs5_read_block(uint32_t block, void* buf);
extern int cfs5_write_block(uint32_t block, const void* buf);

static int cfs5_inode_valid(const cfs5_superblock_t* sb,
                            const cfs5_inode_t* in)
{
    uint64_t extent_blocks = 0;
    if (!sb || !in || in->type > CFS5_INODE_SYMLINK ||
        (in->flags & ~(CFS5_IF_COMPRESSED | CFS5_IF_IMMUTABLE |
                       CFS5_IF_APPENDONLY)) ||
        in->extent_count > CFS5_MAX_EXTENTS)
        return 0;

    if (in->type == CFS5_INODE_FREE)
        return in->size == 0 && in->extent_count == 0;

    for (uint32_t i = 0; i < in->extent_count; i++) {
        const cfs5_extent_t* extent = &in->extents[i];
        if (!extent->block_count ||
            extent->start_block < sb->data_block_start ||
            extent->start_block >= sb->block_count ||
            extent->block_count > sb->block_count - extent->start_block)
            return 0;
        extent_blocks += extent->block_count;
    }

    if (in->xattr_block &&
        (in->xattr_block < sb->data_block_start ||
         in->xattr_block >= sb->block_count))
        return 0;
    if (in->type == CFS5_INODE_FILE &&
        (uint64_t)in->size > extent_blocks * CFS5_BLOCK_SIZE)
        return 0;
    return 1;
}

static void serial_print_uint(uint32_t val) {
    char buf[12];
    int i = 11;
    buf[11] = '\0';
    do {
        buf[--i] = '0' + (val % 10);
        val /= 10;
    } while (val > 0);
    serial_puts(&buf[i]);
}

static int inode_bitmap_find_free(uint8_t* bm, uint32_t bits, uint32_t* out_idx) {
    for (uint32_t i = 0; i < bits; i++) {
        uint32_t byte = i / 8;
        uint8_t  mask = 1 << (i % 8);
        if (!(bm[byte] & mask)) {
            *out_idx = i;
            return 1;
        }
    }
    return 0;
}

int cfs5_alloc_inode(uint32_t* out_ino) {
    cfs5_superblock_t* sb = cfs5_get_super();
    uint8_t buf[CFS5_BLOCK_SIZE];

    if (!sb || !out_ino) return 0;
    uint32_t bits = sb->inode_count;
    uint32_t per_block = CFS5_BLOCK_SIZE * 8;
    uint32_t blocks = sb->free_inode_bitmap_blocks;

    for (uint32_t b = 0; b < blocks; b++) {
        if (!cfs5_read_block(sb->free_inode_bitmap + b, buf))
            return 0;

        uint32_t base = b * per_block;
        if (base >= bits) break;

        uint32_t idx;
        if (!inode_bitmap_find_free(buf, per_block, &idx))
            continue;

        uint32_t global = base + idx;
        if (global >= bits)
            continue;

        uint32_t byte = idx / 8;
        uint8_t  mask = 1 << (idx % 8);
        buf[byte] |= mask;

        if (!cfs5_write_block(sb->free_inode_bitmap + b, buf))
            return 0;

        *out_ino = global;
        return 1;
    }

    return 0;
}

int cfs5_free_inode(uint32_t ino) {
    cfs5_superblock_t* sb = cfs5_get_super();
    if (!sb) return 0;
    if (ino >= sb->inode_count) return 0;

    uint8_t buf[CFS5_BLOCK_SIZE];

    uint32_t per_block = CFS5_BLOCK_SIZE * 8;
    uint32_t b = ino / per_block;
    uint32_t idx = ino % per_block;

    if (b >= sb->free_inode_bitmap_blocks) return 0;

    if (!cfs5_read_block(sb->free_inode_bitmap + b, buf))
        return 0;

    uint32_t byte = idx / 8;
    uint8_t  mask = 1 << (idx % 8);
    buf[byte] &= ~mask;

    if (!cfs5_write_block(sb->free_inode_bitmap + b, buf))
        return 0;

    return 1;
}

int cfs5_read_inode(uint32_t ino, cfs5_inode_t* in) {
    cfs5_superblock_t* sb = cfs5_get_super();
    if (!sb || !in) return 0;
    if (ino >= sb->inode_count) return 0;

    uint32_t inodes_per_block = CFS5_BLOCK_SIZE / sizeof(cfs5_inode_t);
    uint32_t block = sb->inode_table_start + (ino / inodes_per_block);
    uint32_t idx   = ino % inodes_per_block;

    uint8_t buf[CFS5_BLOCK_SIZE];
    if (!cfs5_read_block(block, buf)) return 0;

    cfs5_inode_t* arr = (cfs5_inode_t*)buf;
    if (!cfs5_inode_valid(sb, &arr[idx])) return 0;
    *in = arr[idx];
    return 1;
}


int cfs5_write_inode(uint32_t ino, const cfs5_inode_t* in) {
    cfs5_superblock_t* sb = cfs5_get_super();
    if (!sb || !in || !cfs5_inode_valid(sb, in)) return 0;
    if (ino >= sb->inode_count) return 0;

    uint32_t inodes_per_block = CFS5_BLOCK_SIZE / sizeof(cfs5_inode_t);
    uint32_t block = sb->inode_table_start + (ino / inodes_per_block);
    uint32_t idx   = ino % inodes_per_block;

    uint8_t buf[CFS5_BLOCK_SIZE];
    if (!cfs5_read_block(block, buf))
        return 0;

    cfs5_inode_t* arr = (cfs5_inode_t*)buf;
    arr[idx] = *in;

    if (!cfs5_write_block(block, buf))
        return 0;

    return 1;
}