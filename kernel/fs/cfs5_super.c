// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "fs/cfs5.h"
#include "drivers/ata.h"
#include "lib/string.h"

static cfs5_superblock_t g_cfs5_sb;
static int g_cfs5_mounted = 0;

static int cfs5_region_valid(uint32_t start, uint32_t count,
                             uint32_t block_count)
{
    return count != 0 && start < block_count &&
           count <= block_count - start;
}

static int cfs5_super_valid(const cfs5_superblock_t* sb)
{
    uint64_t inode_capacity;
    uint64_t block_capacity;
    uint64_t inode_table_end;
    uint64_t block_bitmap_end;
    uint64_t inode_bitmap_end;
    uint32_t inodes_per_block = CFS5_BLOCK_SIZE / sizeof(cfs5_inode_t);
    uint64_t required_inode_blocks;

    if (!sb || sb->magic != CFS5_MAGIC || sb->version != CFS5_VERSION ||
        sb->block_size != CFS5_BLOCK_SIZE || !sb->block_count ||
        !sb->inode_count || sb->root_inode >= sb->inode_count)
        return 0;
    if (!inodes_per_block) return 0;

    required_inode_blocks =
        ((uint64_t)sb->inode_count + inodes_per_block - 1) / inodes_per_block;
    if (sb->inode_table_start == 0 ||
        sb->inode_table_blocks < required_inode_blocks ||
        !cfs5_region_valid(sb->inode_table_start, sb->inode_table_blocks,
                           sb->block_count) ||
        !cfs5_region_valid(sb->free_block_bitmap,
                           sb->free_block_bitmap_blocks, sb->block_count) ||
        !cfs5_region_valid(sb->free_inode_bitmap,
                           sb->free_inode_bitmap_blocks, sb->block_count))
        return 0;

    inode_table_end = (uint64_t)sb->inode_table_start +
                      sb->inode_table_blocks;
    block_bitmap_end = (uint64_t)sb->free_block_bitmap +
                       sb->free_block_bitmap_blocks;
    inode_bitmap_end = (uint64_t)sb->free_inode_bitmap +
                       sb->free_inode_bitmap_blocks;
    if (sb->free_block_bitmap < inode_table_end ||
        sb->free_inode_bitmap < block_bitmap_end ||
        sb->data_block_start < inode_bitmap_end ||
        sb->data_block_start >= sb->block_count)
        return 0;

    inode_capacity = (uint64_t)sb->free_inode_bitmap_blocks *
                     CFS5_BLOCK_SIZE * 8;
    block_capacity = (uint64_t)sb->free_block_bitmap_blocks *
                     CFS5_BLOCK_SIZE * 8;
    if (inode_capacity < sb->inode_count ||
        block_capacity < (uint64_t)sb->block_count - sb->data_block_start)
        return 0;

    if ((sb->journal_blocks &&
         !cfs5_region_valid(sb->journal_start, sb->journal_blocks,
                            sb->block_count)) ||
        (sb->snapshot_meta_blocks &&
         !cfs5_region_valid(sb->snapshot_meta_start,
                            sb->snapshot_meta_blocks, sb->block_count)))
        return 0;
    return 1;
}

static int cfs5_read_block_raw(uint32_t block, void* buf) {
    uint64_t lba = (uint64_t)block * (CFS5_BLOCK_SIZE / 512);
    if (!buf) return 0;
    for (int i = 0; i < (CFS5_BLOCK_SIZE / 512); i++) {
        if (!ata_read28(lba + i, (uint8_t*)buf + i * 512))
            return 0;
    }
    return 1;
}

static int cfs5_write_block_raw(uint32_t block, const void* buf) {
    uint64_t lba = (uint64_t)block * (CFS5_BLOCK_SIZE / 512);
    if (!buf) return 0;
    for (int i = 0; i < (CFS5_BLOCK_SIZE / 512); i++) {
        if (!ata_write28(lba + i, (const uint8_t*)buf + i * 512))
            return 0;
    }
    return 1;
}

int cfs5_read_super(cfs5_superblock_t* sb) {
    uint8_t block[CFS5_BLOCK_SIZE];
    if (!sb) return 0;
    if (!cfs5_read_block_raw(0, block))
        return 0;
    memcpy(sb, block, sizeof(*sb));
    if (!cfs5_super_valid(sb)) return 0;
    g_cfs5_sb = *sb;
    return 1;
}

int cfs5_write_super(const cfs5_superblock_t* sb) {
    uint8_t block[CFS5_BLOCK_SIZE] = { 0 };
    if (!cfs5_super_valid(sb)) return 0;
    memcpy(block, sb, sizeof(*sb));
    if (!cfs5_write_block_raw(0, block))
        return 0;
    g_cfs5_sb = *sb;
    return 1;
}

int cfs5_mount(const char* device, const char* mountpoint, int flags, void* data) {
    (void)device; (void)mountpoint; (void)flags; (void)data;

    if (g_cfs5_mounted)
        return 1;  /* Already mounted */

    if (!cfs5_read_super(&g_cfs5_sb))
        return 0;  /* Failed to read superblock */

    g_cfs5_mounted = 1;
    return 1;
}

cfs5_superblock_t* cfs5_get_super(void) {
    if (!g_cfs5_mounted)
        return 0;
    return &g_cfs5_sb;
}