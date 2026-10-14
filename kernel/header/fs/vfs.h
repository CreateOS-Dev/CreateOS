// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_DRIVES 26  // A-Z
#define MAX_MOUNTS 32
#define VFS_MOUNT_READ_ONLY 1

typedef struct {
    uint32_t type;   // FS_TYPE_*
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t size;
} fs_stat_t;

typedef enum {
    FS_TYPE_NONE = 0,
    FS_TYPE_CFS5 = 1,
    FS_TYPE_FAT12 = 2,
    FS_TYPE_FAT16 = 3,
    FS_TYPE_FAT32 = 4,
    FS_TYPE_NTFS = 5,
    FS_TYPE_EXT2 = 6,
    FS_TYPE_ISO9660 = 7,
} fs_type_t;

#define FS_TYPE_NONE  0
#define FS_TYPE_FILE  1
#define FS_TYPE_DIR   2
#define FS_PERM_READ  4
#define FS_PERM_WRITE 2
#define FS_PERM_EXEC  1

typedef struct {
    char device[64];     // Device name (e.g., "ata0", "usb0")
    char mountpoint[64]; // Mount point (e.g., "C:", "/mnt/data")
    fs_type_t type;      // Filesystem type
    int flags;           // Mount flags (RO, etc.)
    void* fs_data;       // Filesystem-specific data
    struct fs_operations* ops; // Filesystem operations
} mount_t;

typedef struct fs_operations {
    int (*mount)(const char* device, const char* mountpoint, int flags, void* data);
    int (*sync)(void* fs_data);
    int (*unmount)(const char* mountpoint);
    int (*stat)(const char* path, fs_stat_t* st);
    int (*read)(const char* path, uint8_t* buf, uint32_t size, uint32_t offset);
    int (*write)(const char* path, const uint8_t* buf, uint32_t size, uint32_t offset);
    int (*list)(const char* path, char names[][64], int max);
    int (*create)(const char* path, uint16_t mode);
    int (*mkdir)(const char* path, uint16_t mode);
    int (*unlink)(const char* path);
    int (*truncate)(const char* path, uint32_t size);
    int (*chmod)(const char* path, uint16_t mode);
    int (*chown)(const char* path, uint16_t uid, uint16_t gid);
} fs_operations_t;

extern mount_t mounts[MAX_MOUNTS];
extern int mount_count;

int vfs_init(void);
int vfs_mount(const char* device, const char* mountpoint, fs_type_t type, int flags, void* data);
int vfs_unmount(const char* mountpoint);
void vfs_forget_device(const char* device);
int vfs_sync(void);
int vfs_stat(const char* path, fs_stat_t* st);
int vfs_read(const char* path, uint8_t* buf, uint32_t size, uint32_t offset);
int vfs_write(const char* path, const uint8_t* buf, uint32_t size, uint32_t offset);
int vfs_list(const char* path, char names[][64], int max);
int vfs_create(const char* path, uint16_t mode);
int vfs_mkdir(const char* path, uint16_t mode);
int vfs_unlink(const char* path);
int vfs_truncate(const char* path, uint32_t size);
fs_type_t vfs_path_type(const char* path);

char get_drive_letter_from_path(const char* path);
const char* get_path_from_drive_letter(char drive_letter, const char* relative_path);
int path_starts_with_drive_letter(const char* path, char* drive_letter, const char** remaining_path);
void initialize_drive_letters(void);

#endif // VFS_H