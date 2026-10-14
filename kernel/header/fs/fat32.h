// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stddef.h>
#include "../lib/string.h"
#include "../lib/stdlib.h"
#include "vfs.h"

#define FAT32_SECTOR_SIZE 512

typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_dir_entries;      /* 0 for FAT32 */
    uint16_t total_sectors_16;      /* 0 if > 65535 */
    uint8_t  media_type;
    uint16_t sectors_per_fat_16;    /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    uint32_t sectors_per_fat;
    uint16_t flags;
    uint16_t version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];            /* "FAT32   " */

    uint8_t  boot_code[420];
    uint16_t boot_sector_signature; /* 0xAA55 */
} fat32_boot_sector_t;

typedef struct __attribute__((packed)) {
    uint32_t lead_signature;        /* 0x41615252 */
    uint8_t  reserved1[480];
    uint32_t struct_signature;      /* 0x61417272 */
    uint32_t free_cluster_count;
    uint32_t next_free_cluster;
    uint8_t  reserved2[12];
    uint32_t trail_signature;       /* 0xAA550000 */
} fat32_fsinfo_t;

typedef struct __attribute__((packed)) {
    char     name[11];              /* 8.3 format, space padded */
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  creation_tenth;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} fat32_dirent_t;

#define FAT32_ATTR_READ_ONLY  0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LONG_NAME  (FAT32_ATTR_READ_ONLY | FAT32_ATTR_HIDDEN | FAT32_ATTR_SYSTEM | FAT32_ATTR_VOLUME_ID)

typedef struct __attribute__((packed)) {
    uint8_t  order;
    uint16_t name1[5];
    uint8_t  attr;                  /* Always 0x0F */
    uint8_t  type;                  /* Always 0x00 */
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t first_cluster;         /* Always 0x0000 */
    uint16_t name3[2];
} fat32_lfn_t;

typedef struct {
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_size;
    uint32_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t sectors_per_fat;
    uint32_t fat_start_lba;
    uint32_t data_start_lba;
    uint32_t root_cluster;
    uint32_t total_clusters;
    uint32_t fat_size_bytes;
    void*    fat_cache;             /* Cached FAT table */
    int      fat_dirty;
} fat32_context_t;

int  fat32_mount(const char* device, const char* mountpoint, int flags, void* data);
int  fat32_unmount(const char* mountpoint);
int  fat32_forget_device(const char* device);
int  fat32_stat(const char* path, fs_stat_t* st);       /* fs_stat_t* */
int  fat32_read(const char* path, uint8_t* buf, uint32_t size, uint32_t offset);
int  fat32_write(const char* path, const uint8_t* buf, uint32_t size, uint32_t offset);
int  fat32_truncate(const char* path, uint32_t size);
int  fat32_list(const char* path, char names[][64], int max);
int  fat32_mkdir(const char* path, uint16_t mode);
int  fat32_create(const char* path, uint16_t mode);
int  fat32_unlink(const char* path);
int  fat32_chmod(const char* path, uint16_t mode);
int  fat32_chown(const char* path, uint16_t uid, uint16_t gid);

int fat32_read_sector(uint32_t lba, void* buf);
int fat32_write_sector(uint32_t lba, const void* buf);
int fat32_read_cluster(uint32_t cluster, void* buf);
int fat32_write_cluster(uint32_t cluster, const void* buf);
uint32_t fat32_get_fat_entry(uint32_t cluster);
int fat32_set_fat_entry(uint32_t cluster, uint32_t value);
uint32_t fat32_alloc_cluster(void);
int fat32_free_cluster(uint32_t cluster);
uint32_t fat32_cluster_to_lba(uint32_t cluster);
int fat32_path_to_cluster(const char* path, uint32_t* out_cluster, int create);
int fat32_find_entry(uint32_t dir_cluster, const char* name, fat32_dirent_t* out);
int fat32_add_entry(uint32_t dir_cluster, const char* name, uint32_t entry_cluster, uint8_t attr, uint32_t size);
int fat32_remove_entry(uint32_t dir_cluster, const char* name);
int fat32_lfn_checksum(const char* name);
void fat32_pack_name(const char* name, char* out);
int fat32_unpack_name(const fat32_dirent_t* entry, char* out);

#endif