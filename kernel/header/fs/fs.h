// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "cfs5.h"
#include "vfs.h"

#define FS_PERM_READ  4
#define FS_PERM_WRITE 2
#define FS_PERM_EXEC  1

int  fs_init(void);

int  fs_stat(const char* path, fs_stat_t* st);
int  fs_mkdir(const char* path, uint16_t mode);
int  fs_create(const char* path, uint16_t mode);
int  fs_unlink(const char* path);
int  fs_read(const char* path, uint8_t* buf, uint32_t size, uint32_t offset);
int  fs_write(const char* path, const uint8_t* buf, uint32_t size, uint32_t offset);
int  fs_list(const char* path, char names[][CFS5_MAX_NAME], int max);
int  fs_list_offset(const char* path, char names[][CFS5_MAX_NAME],
                    uint32_t offset, int max);

int  fs_chmod(const char* path, uint16_t mode);
int  fs_chown(const char* path, uint16_t uid, uint16_t gid);
int  fs_check_access(const char* path, uint16_t uid, uint16_t gid, int want);

uint32_t fs_total_space(void);
uint32_t fs_used_space(void);
int  fs_read_block(uint32_t lba, void* buf);
int  fs_load_file(const char* path, uint8_t* buf, uint32_t max, uint32_t* out_size);
int  fs_write_file(const char* path, const uint8_t* buf, uint32_t size);
int  fs_exists(const char* path);
int  fs_copy(const char* src, const char* dst);
int  fs_rename(const char* src, const char* dst);
int  fs_chdir(const char* path);

typedef struct {
    uint32_t f_type;
    uint32_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint32_t f_fsid;
    uint32_t f_namelen;
} statfs_t;

int  fs_get_statfs(const char* path, statfs_t* buf);
int  fs_unmount(const char* path);

#endif