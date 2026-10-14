// ============================================================
// CreateOS 0.0.1
// ============================================================


#include "fs/fat32.h"
#include "drivers/ata.h"
#include "drivers/block.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "drivers/serial.h"
#include "fs/fs.h"
#include "fs/vfs.h"

#define SERIAL_DEBUG_FAT32 0

#if SERIAL_DEBUG_FAT32
#define fat32_debug(fmt, ...) serial_printf("FAT32: " fmt "\n", ##__VA_ARGS__)
#else
#define fat32_debug(fmt, ...) ((void)0)
#endif

static fat32_context_t g_fat32_ctx;
static int g_fat32_mounted = 0;
static char g_fat32_device[64];

static int fat32_resolve_parent(const char* path, uint32_t* parent_cluster,
                                char name[64]);
static int fat32_valid_short_name(const char* name);
static int fat32_locate_dirent(uint32_t directory_cluster, const char* name,
                               fat32_dirent_t* entry, uint32_t* entry_cluster,
                               uint32_t* entry_index);

int fat32_read_sector(uint32_t lba, void* buf) {
    return block_read_device(g_fat32_device, lba, buf);
}

int fat32_write_sector(uint32_t lba, const void* buf) {
    return block_write_device(g_fat32_device, lba, buf);
}

int fat32_read_cluster(uint32_t cluster, void* buf) {
    if (!g_fat32_mounted) return 0;

    uint32_t lba = fat32_cluster_to_lba(cluster);
    uint32_t sectors = g_fat32_ctx.sectors_per_cluster;

    for (uint32_t i = 0; i < sectors; i++) {
        if (!fat32_read_sector(lba + i, (uint8_t*)buf + i * FAT32_SECTOR_SIZE)) {
            return 0;
        }
    }
    return 1;
}

int fat32_write_cluster(uint32_t cluster, const void* buf) {
    if (!g_fat32_mounted) return 0;

    uint32_t lba = fat32_cluster_to_lba(cluster);
    uint32_t sectors = g_fat32_ctx.sectors_per_cluster;

    for (uint32_t i = 0; i < sectors; i++) {
        if (!fat32_write_sector(lba + i, (const uint8_t*)buf + i * FAT32_SECTOR_SIZE)) {
            return 0;
        }
    }
    return 1;
}

uint32_t fat32_cluster_to_lba(uint32_t cluster) {
    if (cluster < 2) cluster = 2;  /* Cluster 0 and 1 are reserved */
    return g_fat32_ctx.data_start_lba + (cluster - 2) * g_fat32_ctx.sectors_per_cluster;
}

uint32_t fat32_get_fat_entry(uint32_t cluster) {
    if (!g_fat32_mounted || !g_fat32_ctx.fat_cache) return 0x0FFFFFFF;

    if (cluster >= g_fat32_ctx.total_clusters + 2) return 0x0FFFFFFF;

    uint32_t* fat = (uint32_t*)g_fat32_ctx.fat_cache;
    return fat[cluster] & 0x0FFFFFFF;
}

int fat32_set_fat_entry(uint32_t cluster, uint32_t value) {
    if (!g_fat32_mounted || !g_fat32_ctx.fat_cache) return 0;

    if (cluster >= g_fat32_ctx.total_clusters + 2) return 0;

    uint32_t* fat = (uint32_t*)g_fat32_ctx.fat_cache;
    fat[cluster] = (fat[cluster] & 0xF0000000) | (value & 0x0FFFFFFF);
    g_fat32_ctx.fat_dirty = 1;
    return 1;
}

uint32_t fat32_alloc_cluster(void) {
    if (!g_fat32_mounted || !g_fat32_ctx.fat_cache) return 0;

    uint32_t* fat = (uint32_t*)g_fat32_ctx.fat_cache;

    for (uint32_t i = 2; i < g_fat32_ctx.total_clusters + 2; i++) {
        if ((fat[i] & 0x0FFFFFFF) == 0) {
            fat[i] = 0x0FFFFFFF;  /* End of chain marker */
            g_fat32_ctx.fat_dirty = 1;
            return i;
        }
    }
    return 0;  /* No free clusters */
}

int fat32_free_cluster(uint32_t cluster) {
    if (!g_fat32_mounted || !g_fat32_ctx.fat_cache) return 0;

    uint32_t* fat = (uint32_t*)g_fat32_ctx.fat_cache;
    uint32_t next;

    uint32_t traversed = 0;
    while (cluster >= 2 && cluster < g_fat32_ctx.total_clusters + 2 &&
           traversed++ < g_fat32_ctx.total_clusters) {
        next = fat[cluster] & 0x0FFFFFFF;
        fat[cluster] = 0;
        g_fat32_ctx.fat_dirty = 1;

        if (next >= 0x0FFFFFF8) break;  /* End of chain */
        cluster = next;
    }
    return 1;
}

static int fat32_flush_fat(void) {
    if (!g_fat32_mounted || !g_fat32_ctx.fat_cache || !g_fat32_ctx.fat_dirty) return 1;

    uint8_t* fat = (uint8_t*)g_fat32_ctx.fat_cache;
    uint32_t sectors = g_fat32_ctx.sectors_per_fat;

    for (uint8_t f = 0; f < g_fat32_ctx.num_fats; f++) {
        uint32_t fat_lba = g_fat32_ctx.fat_start_lba + f * sectors;
        for (uint32_t s = 0; s < sectors; s++) {
            if (!fat32_write_sector(fat_lba + s, fat + s * FAT32_SECTOR_SIZE)) {
                fat32_debug("Failed to write FAT sector %u", fat_lba + s);
                return 0;
            }
        }
    }

    g_fat32_ctx.fat_dirty = 0;
    return 1;
}

static int fat32_sync(void* fs_data)
{
    (void)fs_data;
    return fat32_flush_fat() && block_flush_device(g_fat32_device);
}

void fat32_pack_name(const char* name, char* out) {
    memset(out, ' ', 11);

    const char* dot = strchr(name, '.');
    size_t name_len = dot ? (size_t)(dot - name) : strlen(name);
    if (name_len > 8) name_len = 8;
    memcpy(out, name, name_len);

    if (dot) {
        const char* ext = dot + 1;
        size_t ext_len = strlen(ext);
        if (ext_len > 3) ext_len = 3;
        memcpy(out + 8, ext, ext_len);
    }

    for (int i = 0; i < 11; i++) {
        if (out[i] >= 'a' && out[i] <= 'z') out[i] -= 32;
    }
}

int fat32_lfn_checksum(const char* name) {
    char packed[11];
    fat32_pack_name(name, packed);

    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = ((sum & 1) << 7) | (sum >> 1);
        sum += packed[i];
    }
    return sum;
}

int fat32_find_entry(uint32_t dir_cluster, const char* name, fat32_dirent_t* out) {
    if (!g_fat32_mounted) return 0;

    char packed_name[11];
    fat32_pack_name(name, packed_name);
    uint8_t lfn_checksum = fat32_lfn_checksum(name);

    uint8_t buf[FAT32_SECTOR_SIZE * 8];  /* Buffer for one cluster */
    uint32_t cluster = dir_cluster;

    while (cluster < 0x0FFFFFF8) {
        if (!fat32_read_cluster(cluster, buf)) return 0;

        fat32_dirent_t* entries = (fat32_dirent_t*)buf;
        int entries_per_cluster = g_fat32_ctx.cluster_size / sizeof(fat32_dirent_t);

        for (int i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) {
                return 0;  /* End of directory */
            }
            if ((uint8_t)entries[i].name[0] == 0xE5) {
                continue;  /* Deleted entry */
            }

            if (entries[i].attr == FAT32_ATTR_LONG_NAME) {
                fat32_lfn_t* lfn = (fat32_lfn_t*)&entries[i];
                if (lfn->checksum == lfn_checksum) {
                    continue;
                }
            }

            if (memcmp(entries[i].name, packed_name, 11) == 0) {
                if (out) *out = entries[i];
                return 1;
            }
        }

        cluster = fat32_get_fat_entry(cluster);
    }

    return 0;
}

int fat32_add_entry(uint32_t dir_cluster, const char* name, uint32_t entry_cluster, uint8_t attr, uint32_t size) {
    if (!g_fat32_mounted) return 0;

    char packed_name[11];
    fat32_pack_name(name, packed_name);

    fat32_dirent_t existing;
    if (fat32_find_entry(dir_cluster, name, &existing)) {
        return 0;  /* Already exists */
    }

    uint8_t buf[FAT32_SECTOR_SIZE * 8];
    uint32_t cluster = dir_cluster;

    while (1) {
        if (cluster < 2 || cluster >= g_fat32_ctx.total_clusters + 2 ||
            !fat32_read_cluster(cluster, buf))
            return 0;

        fat32_dirent_t* entries = (fat32_dirent_t*)buf;
        int entries_per_cluster = g_fat32_ctx.cluster_size / sizeof(fat32_dirent_t);

        for (int i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00 ||
                (uint8_t)entries[i].name[0] == 0xE5) {
                memset(&entries[i], 0, sizeof(fat32_dirent_t));
                memcpy(entries[i].name, packed_name, 11);
                entries[i].attr = attr;
                entries[i].first_cluster_high = (entry_cluster >> 16) & 0xFFFF;
                entries[i].first_cluster_low = entry_cluster & 0xFFFF;
                entries[i].file_size = size;

                entries[i].creation_date = 0;
                entries[i].creation_time = 0;
                entries[i].write_date = 0;
                entries[i].write_time = 0;

                if (!fat32_write_cluster(cluster, buf)) return 0;
                return 1;
            }
        }

        cluster = fat32_get_fat_entry(cluster);
        if (cluster >= 0x0FFFFFF8) break;
    }

    return 0;
}

int fat32_remove_entry(uint32_t dir_cluster, const char* name) {
    if (!g_fat32_mounted) return 0;

    char packed_name[11];
    fat32_pack_name(name, packed_name);

    uint8_t buf[FAT32_SECTOR_SIZE * 8];
    uint32_t cluster = dir_cluster;

    while (cluster < 0x0FFFFFF8) {
        if (!fat32_read_cluster(cluster, buf)) return 0;

        fat32_dirent_t* entries = (fat32_dirent_t*)buf;
        int entries_per_cluster = g_fat32_ctx.cluster_size / sizeof(fat32_dirent_t);

        for (int i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) {
                return 0;  /* Not found */
            }
            if (memcmp(entries[i].name, packed_name, 11) == 0) {
                entries[i].name[0] = 0xE5;
                if (!fat32_write_cluster(cluster, buf)) return 0;
                return 1;
            }
        }

        cluster = fat32_get_fat_entry(cluster);
    }

    return 0;
}

int fat32_path_to_cluster(const char* path, uint32_t* out_cluster, int create) {
    if (!g_fat32_mounted) return 0;

    while (*path == '/') path++;
    if (*path == '\0') {
        *out_cluster = g_fat32_ctx.root_cluster;
        return 1;
    }

    uint32_t cluster = g_fat32_ctx.root_cluster;
    char name[64];

    while (*path) {
        int i = 0;
        while (*path && *path != '/' && i < 63) {
            name[i++] = *path++;
        }
        name[i] = '\0';

        while (*path == '/') path++;

        fat32_dirent_t entry;
        if (!fat32_find_entry(cluster, name, &entry)) {
            if (create && *path == '\0') {
                return 0;
            }
            return 0;
        }

        cluster = ((uint32_t)entry.first_cluster_high << 16) | entry.first_cluster_low;

        if (*path == '\0') {
            *out_cluster = cluster;
            return 1;
        }

        if ((entry.attr & FAT32_ATTR_DIRECTORY) == 0) {
            return 0;  /* Not a directory */
        }
    }

    *out_cluster = cluster;
    return 1;
}

int fat32_mount(const char* device, const char* mountpoint, int flags, void* data) {
    uint64_t sector_count = 0;
    uint64_t total_sectors;
    uint64_t data_start;
    uint64_t cluster_count;
    uint64_t fat_entries;
    size_t device_length;
    (void)mountpoint; (void)flags; (void)data;

    if (g_fat32_mounted || !device) return 0;
    device_length = strlen(device);
    if (!device_length || device_length >= sizeof(g_fat32_device)) return 0;
    if (!block_device_sector_count(device, &sector_count) &&
        strcmp(device, "ata0-master") != 0 &&
        strcmp(device, "default") != 0)
        return 0;
    memset(g_fat32_device, 0, sizeof(g_fat32_device));
    memcpy(g_fat32_device, device, device_length + 1);

    fat32_debug("Mounting FAT32 filesystem");

    fat32_boot_sector_t bs;
    if (!fat32_read_sector(0, &bs)) {
        fat32_debug("Failed to read boot sector");
        g_fat32_device[0] = 0;
        return 0;
    }

    if (bs.bytes_per_sector != FAT32_SECTOR_SIZE) {
        fat32_debug("Unsupported sector size: %u", bs.bytes_per_sector);
        g_fat32_device[0] = 0;
        return 0;
    }

    if (memcmp(bs.fs_type, "FAT32", 5) != 0) {
        fat32_debug("Not a FAT32 filesystem (type: %.8s)", bs.fs_type);
        g_fat32_device[0] = 0;
        return 0;
    }
    total_sectors = bs.total_sectors_32
        ? bs.total_sectors_32 : bs.total_sectors_16;
    data_start = (uint64_t)bs.reserved_sectors +
                 (uint64_t)bs.num_fats * bs.sectors_per_fat;
    if (bs.boot_sector_signature != 0xAA55 ||
        bs.bytes_per_sector != FAT32_SECTOR_SIZE ||
        !bs.sectors_per_cluster ||
        (bs.sectors_per_cluster & (bs.sectors_per_cluster - 1)) ||
        bs.sectors_per_cluster > 128 ||
        !bs.reserved_sectors || !bs.num_fats || bs.num_fats > 2 ||
        !bs.sectors_per_fat || !total_sectors ||
        bs.sectors_per_fat > 0xFFFFFFFFU / FAT32_SECTOR_SIZE ||
        (uint32_t)bs.sectors_per_cluster * FAT32_SECTOR_SIZE > 4096 ||
        data_start >= total_sectors ||
        (sector_count && total_sectors > sector_count)) {
        g_fat32_device[0] = 0;
        return 0;
    }
    cluster_count = (total_sectors - data_start) / bs.sectors_per_cluster;
    fat_entries = (uint64_t)bs.sectors_per_fat *
                  FAT32_SECTOR_SIZE / sizeof(uint32_t);
    if (cluster_count < 1 || cluster_count > 0x0FFFFFF5ULL ||
        fat_entries < cluster_count + 2 ||
        bs.root_cluster < 2 ||
        bs.root_cluster >= cluster_count + 2) {
        g_fat32_device[0] = 0;
        return 0;
    }

    memset(&g_fat32_ctx, 0, sizeof(g_fat32_ctx));
    g_fat32_ctx.bytes_per_sector = bs.bytes_per_sector;
    g_fat32_ctx.sectors_per_cluster = bs.sectors_per_cluster;
    g_fat32_ctx.cluster_size = bs.bytes_per_sector * bs.sectors_per_cluster;
    g_fat32_ctx.reserved_sectors = bs.reserved_sectors;
    g_fat32_ctx.num_fats = bs.num_fats;
    g_fat32_ctx.sectors_per_fat = bs.sectors_per_fat;
    g_fat32_ctx.root_cluster = bs.root_cluster;

    g_fat32_ctx.fat_start_lba = bs.reserved_sectors;
    g_fat32_ctx.data_start_lba = (uint32_t)data_start;
    g_fat32_ctx.total_clusters = (uint32_t)cluster_count;
    g_fat32_ctx.fat_size_bytes = bs.sectors_per_fat * bs.bytes_per_sector;

    fat32_debug("FAT32: cluster_size=%u, fat_start_lba=%u, data_start_lba=%u, total_clusters=%u",
                g_fat32_ctx.cluster_size, g_fat32_ctx.fat_start_lba,
                g_fat32_ctx.data_start_lba, g_fat32_ctx.total_clusters);

    g_fat32_ctx.fat_cache = malloc(g_fat32_ctx.fat_size_bytes);
    if (!g_fat32_ctx.fat_cache) {
        fat32_debug("Failed to allocate FAT cache");
        g_fat32_device[0] = 0;
        return 0;
    }

    uint8_t* fat = (uint8_t*)g_fat32_ctx.fat_cache;
    for (uint32_t s = 0; s < g_fat32_ctx.sectors_per_fat; s++) {
        if (!fat32_read_sector(g_fat32_ctx.fat_start_lba + s, fat + s * FAT32_SECTOR_SIZE)) {
            fat32_debug("Failed to read FAT sector %u", g_fat32_ctx.fat_start_lba + s);
            free(g_fat32_ctx.fat_cache);
            g_fat32_ctx.fat_cache = NULL;
            g_fat32_device[0] = 0;
            return 0;
        }
    }

    g_fat32_mounted = 1;
    fat32_debug("FAT32 mounted successfully");
    return 1;
}

int fat32_unmount(const char* mountpoint) {
    (void)mountpoint;

    if (!g_fat32_mounted) return 1;

    if (!fat32_flush_fat()) return 0;

    if (g_fat32_ctx.fat_cache) {
        free(g_fat32_ctx.fat_cache);
        g_fat32_ctx.fat_cache = NULL;
    }

    g_fat32_mounted = 0;
    g_fat32_device[0] = 0;
    fat32_debug("FAT32 unmounted");
    return 1;
}

int fat32_forget_device(const char* device)
{
    if (!device || !g_fat32_mounted ||
        strcmp(device, g_fat32_device) != 0)
        return 0;
    if (g_fat32_ctx.fat_cache) {
        free(g_fat32_ctx.fat_cache);
        g_fat32_ctx.fat_cache = NULL;
    }
    memset(&g_fat32_ctx, 0, sizeof(g_fat32_ctx));
    g_fat32_mounted = 0;
    g_fat32_device[0] = 0;
    return 1;
}

int fat32_stat(const char* path, fs_stat_t* st) {
    if (!g_fat32_mounted) return -1;
    uint32_t cluster;
    if (!path || !st || !fat32_path_to_cluster(path, &cluster, 0))
        return -1;
    if (cluster == g_fat32_ctx.root_cluster) {
        st->type = FS_TYPE_DIR;
        st->mode = 0755;
        st->uid = 0;
        st->gid = 0;
        st->size = 0;
        return 0;
    }

    uint32_t parent_cluster;
    char name[64];
    fat32_dirent_t entry;
    if (!fat32_resolve_parent(path, &parent_cluster, name) ||
        fat32_locate_dirent(parent_cluster, name, &entry, NULL, NULL) != 1)
        return -1;
    st->type = (entry.attr & FAT32_ATTR_DIRECTORY) ? FS_TYPE_DIR : FS_TYPE_FILE;
    st->mode = (entry.attr & FAT32_ATTR_READ_ONLY) ? 0444 : 0644;
    if (entry.attr & FAT32_ATTR_DIRECTORY) st->mode = 0755;
    st->uid = 0;
    st->gid = 0;
    st->size = entry.file_size;
    return 0;
}

int fat32_read(const char* path, uint8_t* buf, uint32_t size, uint32_t offset) {
    uint32_t parent_cluster;
    uint32_t cluster;
    uint32_t cluster_size;
    uint32_t remaining;
    uint32_t copied = 0;
    char name[64];
    fat32_dirent_t entry;
    uint8_t cluster_buffer[4096];

    if (!g_fat32_mounted || (!buf && size) || size > 0x7FFFFFFFU ||
        !fat32_resolve_parent(path, &parent_cluster, name) ||
        fat32_locate_dirent(parent_cluster, name, &entry, NULL, NULL) != 1 ||
        (entry.attr & FAT32_ATTR_DIRECTORY))
        return -1;
    if (offset >= entry.file_size || !size) return 0;
    if (size > entry.file_size - offset) size = entry.file_size - offset;

    cluster_size = g_fat32_ctx.cluster_size;
    cluster = ((uint32_t)entry.first_cluster_high << 16) |
              entry.first_cluster_low;
    if (!cluster_size || cluster_size > sizeof(cluster_buffer) ||
        cluster < 2 || cluster >= g_fat32_ctx.total_clusters + 2)
        return -1;
    uint32_t clusters_to_skip = offset / cluster_size;
    for (uint32_t i = 0; i < clusters_to_skip; i++) {
        cluster = fat32_get_fat_entry(cluster);
        if (cluster < 2 || cluster >= g_fat32_ctx.total_clusters + 2)
            return -1;
    }

    uint32_t cluster_offset = offset % cluster_size;
    remaining = size;
    while (remaining) {
        if (cluster < 2 || cluster >= g_fat32_ctx.total_clusters + 2 ||
            !fat32_read_cluster(cluster, cluster_buffer))
            return -1;
        uint32_t amount = cluster_size - cluster_offset;
        if (amount > remaining) amount = remaining;
        memcpy(buf + copied, cluster_buffer + cluster_offset, amount);
        copied += amount;
        remaining -= amount;
        cluster_offset = 0;
        if (remaining) {
            cluster = fat32_get_fat_entry(cluster);
            if (cluster < 2 || cluster >= g_fat32_ctx.total_clusters + 2)
                return -1;
        }
    }
    return (int)copied;
}

static int fat32_valid_short_name(const char* name)
{
    size_t base_length = 0;
    size_t extension_length = 0;
    int in_extension = 0;

    if (!name || !*name || strcmp(name, ".") == 0 ||
        strcmp(name, "..") == 0)
        return 0;
    for (const char* p = name; *p; p++) {
        char c = *p;
        if (c == '.') {
            if (in_extension || base_length == 0) return 0;
            in_extension = 1;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || strchr("$%'-_@~!#(){}^`&", c)))
            return 0;
        if (in_extension) {
            if (++extension_length > 3) return 0;
        } else if (++base_length > 8) {
            return 0;
        }
    }
    return !in_extension || extension_length != 0;
}

static int fat32_resolve_parent(const char* path, uint32_t* parent_cluster,
                                char name[64])
{
    char parent[256];
    const char* relative = path;
    const char* last_slash = NULL;
    size_t path_length;
    size_t name_length;

    if (!path || !parent_cluster || !name) return 0;
    path_length = strlen(path);
    if (!path_length || path_length >= sizeof(parent)) return 0;
    while (*relative == '/') relative++;
    if (!*relative) return 0;
    for (const char* p = relative; *p; p++)
        if (*p == '/') last_slash = p;

    if (!last_slash) {
        if (!fat32_path_to_cluster("/", parent_cluster, 0)) return 0;
        name_length = strlen(relative);
        if (!name_length || name_length >= 64) return 0;
        memcpy(name, relative, name_length + 1);
    } else {
        const char* leaf = last_slash + 1;
        size_t prefix_length = (size_t)(last_slash - relative);
        name_length = strlen(leaf);
        if (!name_length || name_length >= 64) return 0;
        memcpy(name, leaf, name_length + 1);
        if (!prefix_length) {
            parent[0] = '/';
            parent[1] = '\0';
        } else {
            if (prefix_length + 2 > sizeof(parent)) return 0;
            parent[0] = '/';
            memcpy(parent + 1, relative, prefix_length);
            parent[prefix_length + 1] = '\0';
        }
        if (!fat32_path_to_cluster(parent, parent_cluster, 0)) return 0;
    }
    return fat32_valid_short_name(name);
}

static int fat32_locate_dirent(uint32_t directory_cluster,
                               const char* name, fat32_dirent_t* entry,
                               uint32_t* entry_cluster,
                               uint32_t* entry_index)
{
    char packed_name[11];
    uint8_t cluster_buffer[4096];
    uint32_t cluster = directory_cluster;
    uint32_t traversed = 0;
    fat32_pack_name(name, packed_name);

    while (cluster >= 2 && cluster < g_fat32_ctx.total_clusters + 2 &&
           traversed++ < g_fat32_ctx.total_clusters) {
        if (!fat32_read_cluster(cluster, cluster_buffer)) return -1;
        fat32_dirent_t* entries = (fat32_dirent_t*)cluster_buffer;
        uint32_t count = g_fat32_ctx.cluster_size / sizeof(*entries);
        for (uint32_t i = 0; i < count; i++) {
            if (entries[i].name[0] == 0) return 0;
            if ((uint8_t)entries[i].name[0] == 0xE5 ||
                entries[i].attr == FAT32_ATTR_LONG_NAME ||
                (entries[i].attr & FAT32_ATTR_VOLUME_ID))
                continue;
            if (memcmp(entries[i].name, packed_name, sizeof(packed_name)) == 0) {
                if (entry) *entry = entries[i];
                if (entry_cluster) *entry_cluster = cluster;
                if (entry_index) *entry_index = i;
                return 1;
            }
        }
        cluster = fat32_get_fat_entry(cluster);
        if (cluster >= 0x0FFFFFF8) return 0;
    }
    return -1;
}

static int fat32_update_dirent(uint32_t directory_cluster,
                               uint32_t entry_index,
                               const fat32_dirent_t* updated)
{
    uint8_t cluster_buffer[4096];
    if (!fat32_read_cluster(directory_cluster, cluster_buffer)) return 0;
    uint32_t entries_per_cluster =
        g_fat32_ctx.cluster_size / sizeof(fat32_dirent_t);
    if (entry_index >= entries_per_cluster) return 0;
    ((fat32_dirent_t*)cluster_buffer)[entry_index] = *updated;
    return fat32_write_cluster(directory_cluster, cluster_buffer);
}

static int fat32_write_range(uint32_t first_cluster, uint32_t offset,
                             const uint8_t* data, uint32_t size,
                             int zero_fill)
{
    uint8_t cluster_buffer[4096];
    uint32_t cluster_size = g_fat32_ctx.cluster_size;
    uint32_t cluster_index = offset / cluster_size;
    uint32_t cluster_offset = offset % cluster_size;
    uint32_t cluster = first_cluster;

    for (uint32_t i = 0; i < cluster_index; i++) {
        if (cluster < 2 || cluster >= g_fat32_ctx.total_clusters + 2)
            return 0;
        cluster = fat32_get_fat_entry(cluster);
        if (cluster >= 0x0FFFFFF8) return 0;
    }
    while (size) {
        uint32_t amount = cluster_size - cluster_offset;
        if (amount > size) amount = size;
        if (cluster < 2 || cluster >= g_fat32_ctx.total_clusters + 2)
            return 0;

        if (cluster_offset == 0 && amount == cluster_size) {
            if (zero_fill)
                memset(cluster_buffer, 0, cluster_size);
            else
                memcpy(cluster_buffer, data, amount);
        } else {
            if (!fat32_read_cluster(cluster, cluster_buffer)) return 0;
            if (zero_fill)
                memset(cluster_buffer + cluster_offset, 0, amount);
            else
                memcpy(cluster_buffer + cluster_offset, data, amount);
        }
        if (!fat32_write_cluster(cluster, cluster_buffer)) return 0;
        if (!zero_fill) data += amount;
        size -= amount;
        cluster_offset = 0;
        if (size) {
            cluster = fat32_get_fat_entry(cluster);
            if (cluster >= 0x0FFFFFF8) return 0;
        }
    }
    return 1;
}

static void fat32_rollback_extension(uint32_t first_new_cluster,
                                     uint32_t original_tail)
{
    if (original_tail)
        (void)fat32_set_fat_entry(original_tail, 0x0FFFFFFF);
    if (first_new_cluster)
        (void)fat32_free_cluster(first_new_cluster);
    (void)fat32_flush_fat();
}

int fat32_write(const char* path, const uint8_t* buf, uint32_t size, uint32_t offset) {
    uint32_t parent_cluster;
    uint32_t entry_cluster = 0;
    uint32_t entry_index = 0;
    uint32_t first_cluster = 0;
    uint32_t original_tail = 0;
    uint32_t chain_tail = 0;
    uint32_t first_new_cluster = 0;
    uint32_t old_size = 0;
    uint32_t new_size;
    uint64_t end_offset = (uint64_t)offset + size;
    uint64_t required_clusters;
    uint32_t existing_clusters = 0;
    uint32_t cluster_size;
    char name[64];
    fat32_dirent_t entry;
    int exists;
    int entry_status;

    if (!g_fat32_mounted || (!buf && size) || size > 0x7FFFFFFFU ||
        end_offset > UINT32_MAX ||
        !fat32_resolve_parent(path, &parent_cluster, name))
        return -1;
    cluster_size = g_fat32_ctx.cluster_size;
    if (!cluster_size || cluster_size > 4096) return -1;

    entry_status = fat32_locate_dirent(parent_cluster, name, &entry,
                                       &entry_cluster, &entry_index);
    if (entry_status < 0) return -1;
    exists = entry_status == 1;
    if (exists) {
        if ((entry.attr & (FAT32_ATTR_DIRECTORY | FAT32_ATTR_READ_ONLY |
                           FAT32_ATTR_VOLUME_ID)) != 0)
            return -1;
        old_size = entry.file_size;
        first_cluster = ((uint32_t)entry.first_cluster_high << 16) |
                        entry.first_cluster_low;
        if (old_size && (first_cluster < 2 ||
                         first_cluster >= g_fat32_ctx.total_clusters + 2))
            return -1;
    } else if (size == 0) {
        if (!fat32_add_entry(parent_cluster, name, 0,
                             FAT32_ATTR_ARCHIVE, 0) ||
            !fat32_sync(NULL))
            return -1;
        return 0;
    }

    new_size = old_size;
    if (end_offset > new_size) new_size = (uint32_t)end_offset;
    required_clusters = new_size
        ? ((uint64_t)new_size + cluster_size - 1) / cluster_size : 0;
    if (required_clusters > g_fat32_ctx.total_clusters) return -1;

    if (first_cluster) {
        uint32_t cluster = first_cluster;
        while (1) {
            if (cluster < 2 || cluster >= g_fat32_ctx.total_clusters + 2 ||
                existing_clusters >= g_fat32_ctx.total_clusters)
                return -1;
            existing_clusters++;
            original_tail = cluster;
            chain_tail = cluster;
            cluster = fat32_get_fat_entry(cluster);
            if (cluster >= 0x0FFFFFF8) break;
            if (cluster < 2 || cluster >= g_fat32_ctx.total_clusters + 2)
                return -1;
        }
    }

    while (existing_clusters < required_clusters) {
        uint8_t zero_cluster[4096] = { 0 };
        uint32_t allocated = fat32_alloc_cluster();
        if (!allocated) {
            fat32_rollback_extension(first_new_cluster, original_tail);
            return -1;
        }
        if (!fat32_write_cluster(allocated, zero_cluster)) {
            (void)fat32_free_cluster(allocated);
            fat32_rollback_extension(first_new_cluster, original_tail);
            return -1;
        }
        if (chain_tail && !fat32_set_fat_entry(chain_tail, allocated)) {
            (void)fat32_free_cluster(allocated);
            fat32_rollback_extension(first_new_cluster, original_tail);
            return -1;
        }
        if (!first_new_cluster) first_new_cluster = allocated;
        if (!first_cluster) first_cluster = allocated;
        chain_tail = allocated;
        existing_clusters++;
    }

    if (size && old_size < offset &&
        !fat32_write_range(first_cluster, old_size, NULL,
                           offset - old_size, 1)) {
        fat32_rollback_extension(first_new_cluster, original_tail);
        return -1;
    }
    if (size && !fat32_write_range(first_cluster, offset, buf, size, 0)) {
        fat32_rollback_extension(first_new_cluster, original_tail);
        return -1;
    }
    if (!fat32_flush_fat()) {
        fat32_rollback_extension(first_new_cluster, original_tail);
        return -1;
    }

    if (exists) {
        entry.first_cluster_high = (uint16_t)(first_cluster >> 16);
        entry.first_cluster_low = (uint16_t)first_cluster;
        entry.file_size = new_size;
        entry.attr |= FAT32_ATTR_ARCHIVE;
        if (!fat32_update_dirent(entry_cluster, entry_index, &entry))
            return -1;
    } else if (!fat32_add_entry(parent_cluster, name, first_cluster,
                                FAT32_ATTR_ARCHIVE, new_size)) {
        return -1;
    }
    return block_flush_device(g_fat32_device) ? (int)size : -1;
}

int fat32_list(const char* path, char names[][64], int max) {
    if (!g_fat32_mounted) return -1;

    uint32_t cluster;
    if (!fat32_path_to_cluster(path, &cluster, 0)) return -1;

    int count = 0;
    uint8_t buf[4096];

    while (cluster < 0x0FFFFFF8 && count < max) {
        if (!fat32_read_cluster(cluster, buf)) break;

        fat32_dirent_t* entries = (fat32_dirent_t*)buf;
        int entries_per_cluster = g_fat32_ctx.cluster_size / sizeof(fat32_dirent_t);

        for (int i = 0; i < entries_per_cluster && count < max; i++) {
            if (entries[i].name[0] == 0x00) {
                goto done;  /* End of directory */
            }
            if ((uint8_t)entries[i].name[0] == 0xE5) continue;  /* Deleted */
            if (entries[i].attr == FAT32_ATTR_LONG_NAME) continue;  /* Skip LFN */
            if (entries[i].attr == FAT32_ATTR_VOLUME_ID) continue;  /* Skip volume label */

            char name[64];
            int pos = 0;

            for (int j = 0; j < 8 && entries[i].name[j] != ' '; j++) {
                char c = entries[i].name[j];
                name[pos++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            }

            if (entries[i].name[8] != ' ') {
                name[pos++] = '.';
                for (int j = 8; j < 11 && entries[i].name[j] != ' '; j++) {
                    char c = entries[i].name[j];
                    name[pos++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
                }
            }

            name[pos] = '\0';
            strcpy(names[count++], name);
        }

        cluster = fat32_get_fat_entry(cluster);
    }

done:
    return count;
}

int fat32_mkdir(const char* path, uint16_t mode) {
    if (!g_fat32_mounted) return -1;
    (void)mode;

    uint32_t parent_cluster;
    char name[64];
    int existing;
    if (!fat32_resolve_parent(path, &parent_cluster, name)) return -1;
    existing = fat32_locate_dirent(parent_cluster, name, NULL, NULL, NULL);
    if (existing != 0) return -1;

    uint32_t new_cluster = fat32_alloc_cluster();
    if (!new_cluster) return -1;

    uint8_t buf[4096];
    memset(buf, 0, g_fat32_ctx.cluster_size);

    fat32_dirent_t* dot = (fat32_dirent_t*)buf;
    memcpy(dot->name, ".          ", 11);
    dot->attr = FAT32_ATTR_DIRECTORY;
    dot->first_cluster_high = (new_cluster >> 16) & 0xFFFF;
    dot->first_cluster_low = new_cluster & 0xFFFF;

    fat32_dirent_t* dotdot = (fat32_dirent_t*)(buf + sizeof(fat32_dirent_t));
    memcpy(dotdot->name, "..         ", 11);
    dotdot->attr = FAT32_ATTR_DIRECTORY;
    dotdot->first_cluster_high = (parent_cluster >> 16) & 0xFFFF;
    dotdot->first_cluster_low = parent_cluster & 0xFFFF;

    if (!fat32_write_cluster(new_cluster, buf)) {
        fat32_free_cluster(new_cluster);
        return -1;
    }

    if (!fat32_add_entry(parent_cluster, name, new_cluster,
                         FAT32_ATTR_DIRECTORY, 0)) {
        fat32_free_cluster(new_cluster);
        (void)fat32_flush_fat();
        return -1;
    }

    return fat32_flush_fat() &&
           block_flush_device(g_fat32_device) ? 0 : -1;
}

int fat32_truncate(const char* path, uint32_t size)
{
    uint32_t parent_cluster;
    uint32_t entry_cluster;
    uint32_t entry_index;
    char name[64];
    fat32_dirent_t entry;
    uint32_t first_cluster;

    if (!g_fat32_mounted || size != 0 ||
        !fat32_resolve_parent(path, &parent_cluster, name) ||
        fat32_locate_dirent(parent_cluster, name, &entry,
                            &entry_cluster, &entry_index) != 1 ||
        (entry.attr & (FAT32_ATTR_DIRECTORY | FAT32_ATTR_READ_ONLY |
                       FAT32_ATTR_VOLUME_ID)))
        return 0;

    first_cluster = ((uint32_t)entry.first_cluster_high << 16) |
                    entry.first_cluster_low;
    entry.first_cluster_high = 0;
    entry.first_cluster_low = 0;
    entry.file_size = 0;
    if (!fat32_update_dirent(entry_cluster, entry_index, &entry) ||
        !block_flush_device(g_fat32_device))
        return 0;
    if (first_cluster &&
        (!fat32_free_cluster(first_cluster) || !fat32_flush_fat()))
        return 0;
    return block_flush_device(g_fat32_device);
}

int fat32_create(const char* path, uint16_t mode) {
    uint32_t parent_cluster;
    char name[64];
    int entry_status;
    (void)mode;
    if (!g_fat32_mounted ||
        !fat32_resolve_parent(path, &parent_cluster, name))
        return -1;
    entry_status = fat32_locate_dirent(parent_cluster, name, NULL, NULL, NULL);
    if (entry_status != 0) return -1;
    if (!fat32_add_entry(parent_cluster, name, 0, FAT32_ATTR_ARCHIVE, 0) ||
        !fat32_sync(NULL))
        return -1;
    return 0;
}

int fat32_unlink(const char* path) {
    if (!g_fat32_mounted) return -1;

    uint32_t parent_cluster;
    char name[64];
    fat32_dirent_t entry;
    int entry_status;

    if (!fat32_resolve_parent(path, &parent_cluster, name))
        return -1;
    entry_status = fat32_locate_dirent(parent_cluster, name, &entry,
                                       NULL, NULL);
    if (entry_status != 1 ||
        (entry.attr & (FAT32_ATTR_DIRECTORY | FAT32_ATTR_READ_ONLY |
                       FAT32_ATTR_VOLUME_ID)))
        return -1;

    uint32_t cluster = ((uint32_t)entry.first_cluster_high << 16) |
                       entry.first_cluster_low;
    if (!fat32_remove_entry(parent_cluster, name) ||
        !block_flush_device(g_fat32_device))
        return -1;
    if (cluster &&
        (!fat32_free_cluster(cluster) || !fat32_flush_fat()))
        return -1;
    return block_flush_device(g_fat32_device) ? 0 : -1;
}

int fat32_chmod(const char* path, uint16_t mode) {
    (void)path; (void)mode;
    return 0;
}

int fat32_chown(const char* path, uint16_t uid, uint16_t gid) {
    (void)path; (void)uid; (void)gid;
    return 0;
}

static fs_operations_t fat32_ops = {
    .mount   = fat32_mount,
    .sync    = fat32_sync,
    .unmount = fat32_unmount,
    .stat    = fat32_stat,
    .read    = fat32_read,
    .write   = fat32_write,
    .list    = fat32_list,
    .create  = fat32_create,
    .mkdir   = fat32_mkdir,
    .unlink  = fat32_unlink,
    .truncate = fat32_truncate,
    .chmod   = fat32_chmod,
    .chown   = fat32_chown,
};

fs_operations_t* get_fat32_ops(void) {
    return &fat32_ops;
}