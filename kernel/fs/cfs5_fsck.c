// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "fs/cfs5.h"

#define CFS5_BITMAP_BITS (CFS5_BLOCK_SIZE * 8)

static uint8_t inode_bitmap[CFS5_BLOCK_SIZE];
static uint8_t block_bitmap[CFS5_BLOCK_SIZE];
static uint8_t directory_block[CFS5_BLOCK_SIZE];
static uint32_t inode_bitmap_index;
static uint32_t block_bitmap_index;
static int inode_bitmap_cached;
static int block_bitmap_cached;

static int cfs5_fsck_bitmap_bit(uint32_t bitmap_start,
                                uint32_t bitmap_blocks, uint32_t bit,
                                uint8_t* bitmap, uint32_t* cached_index,
                                int* cached)
{
    uint32_t index = bit / CFS5_BITMAP_BITS;

    if (index >= bitmap_blocks)
        return -1;
    if (!*cached || *cached_index != index) {
        if (!cfs5_read_block(bitmap_start + index, bitmap))
            return -1;
        *cached_index = index;
        *cached = 1;
    }
    return (bitmap[(bit % CFS5_BITMAP_BITS) / 8] >>
            (bit % 8)) & 1;
}

static int cfs5_fsck_inode_allocated(const cfs5_superblock_t* sb,
                                     uint32_t inode)
{
    return cfs5_fsck_bitmap_bit(sb->free_inode_bitmap,
                                sb->free_inode_bitmap_blocks, inode,
                                inode_bitmap, &inode_bitmap_index,
                                &inode_bitmap_cached);
}

static int cfs5_fsck_block_allocated(const cfs5_superblock_t* sb,
                                     uint32_t block)
{
    if (block < sb->data_block_start || block >= sb->block_count)
        return 0;
    return cfs5_fsck_bitmap_bit(sb->free_block_bitmap,
                                sb->free_block_bitmap_blocks,
                                block - sb->data_block_start, block_bitmap,
                                &block_bitmap_index, &block_bitmap_cached);
}

static int cfs5_fsck_check_inode_blocks(const cfs5_superblock_t* sb,
                                        const cfs5_inode_t* inode)
{
    uint64_t capacity = 0;

    for (uint32_t i = 0; i < inode->extent_count; i++) {
        const cfs5_extent_t* extent = &inode->extents[i];
        uint64_t end = (uint64_t)extent->start_block + extent->block_count;

        if (!extent->block_count || extent->start_block < sb->data_block_start ||
            end > sb->block_count)
            return 0;
        capacity += (uint64_t)extent->block_count * CFS5_BLOCK_SIZE;
        for (uint32_t j = 0; j < i; j++) {
            const cfs5_extent_t* previous = &inode->extents[j];
            uint64_t previous_end =
                (uint64_t)previous->start_block + previous->block_count;
            if (extent->start_block < previous_end &&
                previous->start_block < end)
                return 0;
        }
        for (uint32_t block = extent->start_block; block < end; block++) {
            if (cfs5_fsck_block_allocated(sb, block) != 1)
                return 0;
        }
    }
    return inode->size <= capacity;
}

static int cfs5_fsck_check_directory(const cfs5_superblock_t* sb,
                                     const cfs5_inode_t* inode)
{
    uint32_t entries_per_block = CFS5_BLOCK_SIZE / sizeof(cfs5_dirent_t);

    for (uint32_t i = 0; i < inode->extent_count; i++) {
        const cfs5_extent_t* extent = &inode->extents[i];
        for (uint32_t block = 0; block < extent->block_count; block++) {
            if (!cfs5_read_block(extent->start_block + block, directory_block))
                return 0;
            cfs5_dirent_t* entries = (cfs5_dirent_t*)directory_block;
            for (uint32_t j = 0; j < entries_per_block; j++) {
                cfs5_dirent_t* entry = &entries[j];
                uint32_t target_allocated;
                int name_terminated = 0;

                if (!entry->inode)
                    continue;
                if (entry->inode >= sb->inode_count)
                    return 0;
                for (uint32_t k = 0; k < CFS5_MAX_NAME; k++) {
                    if (!entry->name[k]) {
                        name_terminated = k != 0;
                        break;
                    }
                }
                if (!name_terminated)
                    return 0;
                target_allocated = (uint32_t)cfs5_fsck_inode_allocated(
                    sb, entry->inode);
                if (target_allocated != 1)
                    return 0;
                cfs5_inode_t target;
                if (!cfs5_read_inode(entry->inode, &target) ||
                    target.type == CFS5_INODE_FREE)
                    return 0;
            }
        }
    }
    return 1;
}

int cfs5_fsck_run(void)
{
    cfs5_superblock_t* sb = cfs5_get_super();
    cfs5_inode_t inode;
    int root_found = 0;

    if (!sb)
        return 0;

    inode_bitmap_cached = 0;
    block_bitmap_cached = 0;
    for (uint32_t ino = 0; ino < sb->inode_count; ino++) {
        int allocated = cfs5_fsck_inode_allocated(sb, ino);

        if (allocated < 0 || !cfs5_read_inode(ino, &inode))
            return 0;
        if (ino == sb->root_inode) {
            if (allocated != 1 || inode.type != CFS5_INODE_DIR)
                return 0;
            root_found = 1;
        } else if (!ino) {
            if (allocated != 1 || inode.type != CFS5_INODE_FREE)
                return 0;
            continue;
        }
        if (allocated != (inode.type != CFS5_INODE_FREE))
            return 0;
        if (!allocated)
            continue;
        if (!cfs5_fsck_check_inode_blocks(sb, &inode))
            return 0;
        if (inode.xattr_block &&
            cfs5_fsck_block_allocated(sb, inode.xattr_block) != 1)
            return 0;
        if (inode.type == CFS5_INODE_DIR &&
            !cfs5_fsck_check_directory(sb, &inode))
            return 0;
    }
    return root_found;
}
