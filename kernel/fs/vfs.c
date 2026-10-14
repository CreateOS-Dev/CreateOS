// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "fs/vfs.h"
#include "fs/fs.h"
#include "fs/cfs5.h"
#include "fs/fat32.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "drivers/ata.h"
#include "drivers/block.h"
#include "drivers/serial.h"
#include <stddef.h>

#define SERIAL_DEBUG_VFS 0

#if SERIAL_DEBUG_VFS
#define vfs_debug(fmt, ...) serial_printf("VFS: " fmt "\n", ##__VA_ARGS__)
#else
#define vfs_debug(fmt, ...) ((void)0)
#endif

static inline int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

static fs_operations_t cfs5_ops = {
    .mount   = NULL, /* handled specially in vfs_mount */
    .sync    = NULL,
    .unmount = NULL,
    .stat    = fs_stat,
    .read    = fs_read,
    .write   = fs_write,
    .list    = fs_list,
    .create  = NULL,
    .chmod   = fs_chmod,
    .chown   = fs_chown,
};

static int cfs5_sync(void* fs_data)
{
    (void)fs_data;
    return block_flush();
}

int vfs_sync(void)
{
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_t* mount = &mounts[i];
        if (mount->type == FS_TYPE_NONE) continue;
        if (mount->ops == &cfs5_ops) {
            if (!cfs5_sync(mount->fs_data)) return 0;
        } else if (!mount->ops || !mount->ops->sync ||
                   !mount->ops->sync(mount->fs_data)) {
            return 0;
        }
    }
    return block_flush();
}

extern fs_operations_t* get_fat32_ops(void);

mount_t mounts[MAX_MOUNTS];
int mount_count = 0;

int vfs_init(void) {
    vfs_debug("Initializing VFS");

    for (int i = 0; i < MAX_MOUNTS; i++) {
        memset(&mounts[i], 0, sizeof(mount_t));
    }
    mount_count = 0;

    if (vfs_mount("ata0-master", "/", FS_TYPE_CFS5, 0, NULL) != 0) {
        vfs_debug("Failed to mount CFS5 at /");
        return -1;
    }
    if (vfs_mount("ata0-master", "C:", FS_TYPE_CFS5, 0, NULL) != 0) {
        vfs_debug("Failed to mount CFS5 at C:");
    }

    vfs_debug("VFS initialized");
    return 0;
}

static int find_free_mount_slot(void) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].type == FS_TYPE_NONE) {
            return i;
        }
    }
    return -1;
}

static int find_mount_by_path(const char* path) {
    int best_match = -1;
    int longest_prefix = 0;

    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].type != FS_TYPE_NONE) {
            int prefix_len = strlen(mounts[i].mountpoint);
            if (prefix_len > 0 &&
                strncmp(path, mounts[i].mountpoint, prefix_len) == 0 &&
                (path[prefix_len] == '\0' || path[prefix_len] == '/' ||
                 mounts[i].mountpoint[prefix_len - 1] == '/')) {
                if (prefix_len > longest_prefix) {
                    longest_prefix = prefix_len;
                    best_match = i;
                }
            }
        }
    }

    return best_match;
}

static const char* normalize_vfs_path(const char* path, char full_path[256])
{
    char drive_letter;
    const char* relative_path;
    if (!path) return NULL;
    if (path_starts_with_drive_letter(path, &drive_letter, &relative_path)) {
        const char* normalized =
            get_path_from_drive_letter(drive_letter, relative_path);
        if (!normalized || strlen(normalized) >= 256) return NULL;
        strcpy(full_path, normalized);
        return full_path;
    }
    return path;
}

fs_type_t vfs_path_type(const char* path)
{
    char full_path[256];
    const char* normalized = normalize_vfs_path(path, full_path);
    int index;
    if (!normalized) return FS_TYPE_NONE;
    index = find_mount_by_path(normalized);
    return index < 0 ? FS_TYPE_NONE : mounts[index].type;
}

static void make_absolute_path(const char* mountpoint, const char* relative_path, char* abs_path, size_t size) {
    if (relative_path[0] == '/') {
        sprintf(abs_path, "%s%s", mountpoint, relative_path);
    } else {
        sprintf(abs_path, "%s/%s", mountpoint, relative_path);
    }
}

char get_drive_letter_from_path(const char* path) {
    if (path && is_alpha(path[0]) && path[1] == ':') {
        return to_upper(path[0]);
    }
    return 0; /* Not a drive letter path */
}

const char* get_path_from_drive_letter(char drive_letter, const char* relative_path) {
    static char path[256];
    drive_letter = to_upper(drive_letter);
    if (drive_letter >= 'A' && drive_letter <= 'Z') {
        if (relative_path && relative_path[0] != '\0') {
            sprintf(path, "%c:/%s", drive_letter, relative_path);
        } else {
            sprintf(path, "%c:/", drive_letter);
        }
        return path;
    }
    return NULL;
}

int path_starts_with_drive_letter(const char* path, char* drive_letter, const char** remaining_path) {
    if (path && is_alpha(path[0]) && path[1] == ':') {
        *drive_letter = to_upper(path[0]);
        if (remaining_path) {
            *remaining_path = &path[2]; /* Skip "X:" */
            if (**remaining_path == '/' || **remaining_path == '\\') {
                (*remaining_path)++;
            }
        }
        return 1;
    }
    return 0;
}

void initialize_drive_letters(void) {
    /* In a real system, we would detect hardware and assign letters accordingly.
     * For now, we note that C: is our primary drive.
     */
    vfs_debug("Drive letters initialized (A:-Floppy, C:-HardDisk)");
}

int vfs_mount(const char* device, const char* mountpoint, fs_type_t type, int flags, void* data) {
    vfs_debug("Mounting %s as type %d at %s", device, type, mountpoint);

    int existing = find_mount_by_path(mountpoint);
    if (existing >= 0) {
        vfs_debug("Mountpoint %s already in use", mountpoint);
        return -1; /* Already mounted */
    }

    int slot = find_free_mount_slot();
    if (slot < 0) {
        vfs_debug("No free mount slots");
        return -2; /* No space */
    }

    mount_t* mount = &mounts[slot];
    memset(mount, 0, sizeof(mount_t));
    strncpy(mount->device, device, sizeof(mount->device) - 1);
    strncpy(mount->mountpoint, mountpoint, sizeof(mount->mountpoint) - 1);
    mount->type = type;
    mount->flags = flags;

    if (type == FS_TYPE_CFS5) {
        if (flags & VFS_MOUNT_READ_ONLY) {
            memset(mount, 0, sizeof(mount_t));
            return -3;
        }
        if (strcmp(device, "ata0-master") != 0) {
            vfs_debug("Unsupported device for CFS5: %s", device);
            memset(mount, 0, sizeof(mount_t)); /* Clear the slot */
            return -3; /* Unsupported filesystem/device */
        }
        mount->ops = &cfs5_ops;
    } else if (type == FS_TYPE_FAT32) {
        fs_operations_t* fat32_ops = get_fat32_ops();
        if (!fat32_ops) {
            vfs_debug("FAT32 operations not available");
            memset(mount, 0, sizeof(mount_t));
            return -3;
        }
        mount->ops = fat32_ops;
    } else {
        /* Unknown filesystem type */
        memset(mount, 0, sizeof(mount_t));
        return -3;
    }

    if (mount->ops->mount &&
        !mount->ops->mount(device, mountpoint, flags, data)) {
        vfs_debug("Filesystem mount failed for %s at %s", device, mountpoint);
        memset(mount, 0, sizeof(mount_t));
        return -4;
    }

    if (existing < 0) {
        mount_count++;
    }

    vfs_debug("Mount successful at slot %d", slot);
    return 0;
}

int vfs_unmount(const char* mountpoint) {
    vfs_debug("Unmounting %s", mountpoint);

    if (!mountpoint || !strcmp(mountpoint, "/")) return -1;

    int idx = find_mount_by_path(mountpoint);
    if (idx < 0) {
        vfs_debug("Mountpoint %s not found", mountpoint);
        return -1; /* Not found */
    }

    mount_t* mount = &mounts[idx];

    if (mount->ops->unmount) {
        if (!mount->ops->sync || !mount->ops->sync(mount->fs_data))
            return -1;
        int ret = mount->ops->unmount(mountpoint);
        if (ret <= 0) {
            vfs_debug("Filesystem unmount failed: %d", ret);
            return ret < 0 ? ret : -1;
        }
    }

    else if (mount->ops == &cfs5_ops && !cfs5_sync(mount->fs_data))
        return -1;
    else if (mount->ops != &cfs5_ops)
        return -1;

    memset(mount, 0, sizeof(mount_t));
    mount_count--;

    vfs_debug("Unmount successful");
    return 0;
}

void vfs_forget_device(const char* device)
{
    if (!device || !device[0]) return;
    for (int i = 0; i < MAX_MOUNTS; i++) {
        mount_t* mount = &mounts[i];
        if (mount->type == FS_TYPE_NONE ||
            strcmp(mount->device, device) != 0)
            continue;
        if (mount->type == FS_TYPE_FAT32)
            (void)fat32_forget_device(device);
        memset(mount, 0, sizeof(*mount));
        if (mount_count > 0) mount_count--;
    }
}

int vfs_stat(const char* path, fs_stat_t* st) {
    char drive_letter;
    const char* relative_path;
    if (path_starts_with_drive_letter(path, &drive_letter, &relative_path)) {
        char full_path[256];
        const char* base = get_path_from_drive_letter(drive_letter, "");
        if (base) {
            if (relative_path && *relative_path) {
                sprintf(full_path, "%s/%s", base, relative_path);
            } else {
                strcpy(full_path, base);
            }
            path = full_path;
        }
    }

    int idx = find_mount_by_path(path);
    if (idx < 0) {
        return -1; /* No matching mount */
    }

    mount_t* mount = &mounts[idx];
    if (!mount->ops->stat) {
        return -2; /* Operation not supported */
    }

    char relative_to_mount[256];
    if (strncmp(path, mount->mountpoint, strlen(mount->mountpoint)) == 0) {
        strcpy(relative_to_mount, path + strlen(mount->mountpoint));
        if (relative_to_mount[0] == '\0') {
            strcpy(relative_to_mount, "/"); /* Root of mount */
        }
    } else {
        strcpy(relative_to_mount, path); /* Fallback */
    }

    return mount->ops->stat(relative_to_mount, st);
}

int vfs_read(const char* path, uint8_t* buf, uint32_t size, uint32_t offset) {
    char drive_letter;
    const char* relative_path;
    if (path_starts_with_drive_letter(path, &drive_letter, &relative_path)) {
        char full_path[256];
        const char* base = get_path_from_drive_letter(drive_letter, "");
        if (base) {
            if (relative_path && *relative_path) {
                sprintf(full_path, "%s/%s", base, relative_path);
            } else {
                strcpy(full_path, base);
            }
            path = full_path;
        }
    }

    int idx = find_mount_by_path(path);
    if (idx < 0) {
        return -1; /* No matching mount */
    }

    mount_t* mount = &mounts[idx];
    if (!mount->ops->read) {
        return -2; /* Operation not supported */
    }

    char relative_to_mount[256];
    if (strncmp(path, mount->mountpoint, strlen(mount->mountpoint)) == 0) {
        strcpy(relative_to_mount, path + strlen(mount->mountpoint));
        if (relative_to_mount[0] == '\0') {
            strcpy(relative_to_mount, "/"); /* Root of mount */
        }
    } else {
        strcpy(relative_to_mount, path); /* Fallback */
    }

    return mount->ops->read(relative_to_mount, buf, size, offset);
}

int vfs_write(const char* path, const uint8_t* buf, uint32_t size, uint32_t offset) {
    char drive_letter;
    const char* relative_path;
    if (path_starts_with_drive_letter(path, &drive_letter, &relative_path)) {
        char full_path[256];
        const char* base = get_path_from_drive_letter(drive_letter, "");
        if (base) {
            if (relative_path && *relative_path) {
                sprintf(full_path, "%s/%s", base, relative_path);
            } else {
                strcpy(full_path, base);
            }
            path = full_path;
        }
    }

    int idx = find_mount_by_path(path);
    if (idx < 0) {
        return -1; /* No matching mount */
    }

    mount_t* mount = &mounts[idx];
    if (mount->flags & VFS_MOUNT_READ_ONLY) return -1;
    if (!mount->ops->write) {
        return -2; /* Operation not supported */
    }

    char relative_to_mount[256];
    if (strncmp(path, mount->mountpoint, strlen(mount->mountpoint)) == 0) {
        strcpy(relative_to_mount, path + strlen(mount->mountpoint));
        if (relative_to_mount[0] == '\0') {
            strcpy(relative_to_mount, "/"); /* Root of mount */
        }
    } else {
        strcpy(relative_to_mount, path); /* Fallback */
    }

    return mount->ops->write(relative_to_mount, buf, size, offset);
}

int vfs_list(const char* path, char names[][64], int max) {
    char drive_letter;
    const char* relative_path;
    if (path_starts_with_drive_letter(path, &drive_letter, &relative_path)) {
        char full_path[256];
        const char* base = get_path_from_drive_letter(drive_letter, "");
        if (base) {
            if (relative_path && *relative_path) {
                sprintf(full_path, "%s/%s", base, relative_path);
            } else {
                strcpy(full_path, base);
            }
            path = full_path;
        }
    }

    int idx = find_mount_by_path(path);
    if (idx < 0) {
        return -1; /* No matching mount */
    }

    mount_t* mount = &mounts[idx];
    if (!mount->ops->list) {
        return -2; /* Operation not supported */
    }

    char relative_to_mount[256];
    if (strncmp(path, mount->mountpoint, strlen(mount->mountpoint)) == 0) {
        strcpy(relative_to_mount, path + strlen(mount->mountpoint));
        if (relative_to_mount[0] == '\0') {
            strcpy(relative_to_mount, "/"); /* Root of mount */
        }
    } else {
        strcpy(relative_to_mount, path); /* Fallback */
    }

    return mount->ops->list(relative_to_mount, names, max);
}

int vfs_create(const char* path, uint16_t mode)
{
    char full_path[256];
    const char* normalized = normalize_vfs_path(path, full_path);
    int index;
    mount_t* mount;
    char relative_to_mount[256];
    size_t prefix_length;

    if (!normalized || (index = find_mount_by_path(normalized)) < 0)
        return -1;
    mount = &mounts[index];
    if (mount->flags & VFS_MOUNT_READ_ONLY) return -1;
    if (!mount->ops->create) return -2;

    prefix_length = strlen(mount->mountpoint);
    if (prefix_length >= sizeof(relative_to_mount)) return -1;
    strcpy(relative_to_mount, normalized + prefix_length);
    if (!relative_to_mount[0]) strcpy(relative_to_mount, "/");
    return mount->ops->create(relative_to_mount, mode);
}

int vfs_mkdir(const char* path, uint16_t mode)
{
    char full_path[256];
    const char* normalized = normalize_vfs_path(path, full_path);
    int index;
    mount_t* mount;
    char relative_to_mount[256];
    size_t prefix_length;

    if (!normalized || (index = find_mount_by_path(normalized)) < 0)
        return -1;
    mount = &mounts[index];
    if (mount->flags & VFS_MOUNT_READ_ONLY) return -1;
    if (!mount->ops->mkdir) return -2;

    prefix_length = strlen(mount->mountpoint);
    if (prefix_length >= sizeof(relative_to_mount)) return -1;
    strcpy(relative_to_mount, normalized + prefix_length);
    if (!relative_to_mount[0]) strcpy(relative_to_mount, "/");
    return mount->ops->mkdir(relative_to_mount, mode);
}

int vfs_unlink(const char* path)
{
    char full_path[256];
    const char* normalized = normalize_vfs_path(path, full_path);
    int index;
    mount_t* mount;
    char relative_to_mount[256];
    size_t prefix_length;

    if (!normalized || (index = find_mount_by_path(normalized)) < 0)
        return -1;
    mount = &mounts[index];
    if (mount->flags & VFS_MOUNT_READ_ONLY) return -1;
    if (!mount->ops->unlink) return -2;

    prefix_length = strlen(mount->mountpoint);
    if (prefix_length >= sizeof(relative_to_mount)) return -1;
    strcpy(relative_to_mount, normalized + prefix_length);
    if (!relative_to_mount[0]) strcpy(relative_to_mount, "/");
    return mount->ops->unlink(relative_to_mount);
}

int vfs_truncate(const char* path, uint32_t size)
{
    char full_path[256];
    const char* normalized = normalize_vfs_path(path, full_path);
    int index;
    mount_t* mount;
    char relative_to_mount[256];
    size_t prefix_length;

    if (!normalized || (index = find_mount_by_path(normalized)) < 0)
        return -1;
    mount = &mounts[index];
    if (mount->flags & VFS_MOUNT_READ_ONLY) return -1;
    if (!mount->ops->truncate) return -2;

    prefix_length = strlen(mount->mountpoint);
    if (prefix_length >= sizeof(relative_to_mount)) return -1;
    strcpy(relative_to_mount, normalized + prefix_length);
    if (!relative_to_mount[0]) strcpy(relative_to_mount, "/");
    return mount->ops->truncate(relative_to_mount, size);
}