// ============================================================
// CreateOS 0.0.1
// ============================================================


#ifndef CFS5_H
#define CFS5_H

#include <stdint.h>

#define CFS5_MAGIC          0x43534635  // "CSF5"
#define CFS5_VERSION        1

#define CFS5_BLOCK_SIZE     4096
#define CFS5_MAX_NAME       64
#define CFS5_MAX_EXTENTS    8

#define CFS5_INODE_FREE     0
#define CFS5_INODE_FILE     1
#define CFS5_INODE_DIR      2
#define CFS5_INODE_SYMLINK  3

#define CFS5_S_IRUSR  0400
#define CFS5_S_IWUSR  0200
#define CFS5_S_IXUSR  0100
#define CFS5_S_IRGRP  0040
#define CFS5_S_IWGRP  0020
#define CFS5_S_IXGRP  0010
#define CFS5_S_IROTH  0004
#define CFS5_S_IWOTH  0002
#define CFS5_S_IXOTH  0001

#define CFS5_IF_COMPRESSED  0x0001
#define CFS5_IF_IMMUTABLE   0x0002
#define CFS5_IF_APPENDONLY  0x0004

typedef struct {
    uint32_t start_block;
    uint32_t block_count;
} __attribute__((packed)) cfs5_extent_t;

typedef struct {
    uint16_t type;
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint16_t flags;

    uint32_t size;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;

    uint8_t  extent_count;
    uint8_t  reserved[3];
    cfs5_extent_t extents[CFS5_MAX_EXTENTS];

    uint32_t xattr_block;
} __attribute__((packed)) cfs5_inode_t;

typedef struct {
    uint32_t inode;
    char     name[CFS5_MAX_NAME];
} __attribute__((packed)) cfs5_dirent_t;

typedef struct {
    uint32_t magic;
    uint32_t version;

    uint32_t block_size;
    uint32_t block_count;

    uint32_t inode_count;
    uint32_t inode_table_start;
    uint32_t inode_table_blocks;

    uint32_t data_block_start;

    uint32_t free_block_bitmap;
    uint32_t free_block_bitmap_blocks;

    uint32_t free_inode_bitmap;
    uint32_t free_inode_bitmap_blocks;

    uint32_t journal_start;
    uint32_t journal_blocks;

    uint32_t snapshot_meta_start;
    uint32_t snapshot_meta_blocks;

    uint32_t root_inode;

    uint32_t flags;
    uint32_t checksum;
} __attribute__((packed)) cfs5_superblock_t;

int  cfs5_read_super(cfs5_superblock_t* sb);
int  cfs5_write_super(const cfs5_superblock_t* sb);

int  cfs5_mount(const char* device, const char* mountpoint, int flags, void* data);
cfs5_superblock_t* cfs5_get_super(void);

int  cfs5_read_block(uint32_t block, void* buf);
int  cfs5_write_block(uint32_t block, const void* buf);

int  cfs5_alloc_block(uint32_t* out_block);
int  cfs5_free_block(uint32_t block);

int  cfs5_alloc_inode(uint32_t* out_ino);
int  cfs5_free_inode(uint32_t ino);

int  cfs5_read_inode(uint32_t ino, cfs5_inode_t* in);
int  cfs5_write_inode(uint32_t ino, const cfs5_inode_t* in);

int  cfs5_fsck_run(void);

#endif
