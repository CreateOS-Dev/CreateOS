// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "fs/fs.h"
#include "fs/cfs5.h"
#include "fs/fat32.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "drivers/serial.h"
#include <stddef.h>
#include "fs/vfs.h"

extern int cfs5_mount(const char* device, const char* mountpoint, int flags, void* data);
extern int cfs5_read_inode(uint32_t ino, cfs5_inode_t* in);
extern int cfs5_write_inode(uint32_t ino, const cfs5_inode_t* in);
extern int cfs5_alloc_inode(uint32_t* out_ino);
extern int cfs5_free_inode(uint32_t ino);
extern int cfs5_alloc_block(uint32_t* out_block);
extern int cfs5_free_block(uint32_t block);
extern int cfs5_read_block(uint32_t block, void* buf);
extern int cfs5_write_block(uint32_t block, const void* buf);
extern cfs5_superblock_t* cfs5_get_super();

static uint32_t g_root_ino = 0;

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

int fs_init(void) {
    serial_puts("[fs] Initializing VFS\n");
    vfs_init();

    if (!cfs5_mount(NULL, NULL, 0, NULL)) {
        serial_puts("[fs] ERROR: CFS5 mount failed\n");
        return 0;
    }
    cfs5_superblock_t* sb = cfs5_get_super();
    g_root_ino = sb->root_inode;
    serial_puts("[fs] CFS5 mounted, root inode=");
    serial_print_uint(g_root_ino);
    serial_puts("\n");
    return 1;
}

static int cfs5_name_valid(const char* name)
{
    if (!name) return 0;
    for (uint32_t i = 0; i < CFS5_MAX_NAME; i++) {
        if (!name[i]) return i != 0;
    }
    return 0;
}

static int cfs5_dirent_valid(const cfs5_dirent_t* entry)
{
    cfs5_superblock_t* sb = cfs5_get_super();
    if (!entry || !sb || !entry->inode || entry->inode >= sb->inode_count)
        return 0;
    for (uint32_t i = 0; i < CFS5_MAX_NAME; i++) {
        if (!entry->name[i]) return i != 0;
    }
    return 0;
}

static int cfs5_dirent_name_equal(const cfs5_dirent_t* entry,
                                  const char* name)
{
    for (uint32_t i = 0; i < CFS5_MAX_NAME; i++) {
        if (entry->name[i] != name[i]) return 0;
        if (!name[i]) return 1;
    }
    return 0;
}

static int dir_find_entry(uint32_t dir_ino, const char* name, uint32_t* out_ino, uint32_t* out_blk, int* out_idx) {
    cfs5_inode_t dir;
    if (!cfs5_name_valid(name)) return 0;
    if (!cfs5_read_inode(dir_ino, &dir))
        return 0;
    if (dir.type != CFS5_INODE_DIR)
        return 0;

    uint8_t buf[CFS5_BLOCK_SIZE];
    for (int e = 0; e < dir.extent_count; e++) {
        cfs5_extent_t* ex = &dir.extents[e];
        for (uint32_t b = 0; b < ex->block_count; b++) {
            uint32_t blk = ex->start_block + b;
            if (!cfs5_read_block(blk, buf))
                return 0;

            cfs5_dirent_t* ents = (cfs5_dirent_t*)buf;
            int max = CFS5_BLOCK_SIZE / sizeof(cfs5_dirent_t);
            for (int i = 0; i < max; i++) {
                if (ents[i].inode == 0)
                    continue;
                if (!cfs5_dirent_valid(&ents[i])) {
                    serial_puts("[fs] ERROR: malformed CFS5 directory entry\n");
                    return 0;
                }
                if (cfs5_dirent_name_equal(&ents[i], name)) {
                    if (out_ino) *out_ino = ents[i].inode;
                    if (out_blk) *out_blk = blk;
                    if (out_idx) *out_idx = i;
                    return 1;
                }
            }
        }
    }

    return 0;
}

static int dir_add_entry(uint32_t dir_ino, const char* name, uint32_t child_ino) {
    cfs5_inode_t dir;
    cfs5_superblock_t* sb = cfs5_get_super();
    if (!sb || !cfs5_name_valid(name) || !child_ino ||
        child_ino >= sb->inode_count)
        return 0;
    if (!cfs5_read_inode(dir_ino, &dir))
        return 0;
    if (dir.type != CFS5_INODE_DIR)
        return 0;

    uint8_t buf[CFS5_BLOCK_SIZE];

    if (dir.extent_count == 0) {
        uint32_t blk;
        if (!cfs5_alloc_block(&blk))
            return 0;
        dir.extents[0].start_block = blk;
        dir.extents[0].block_count = 1;
        dir.extent_count = 1;
        memset(buf, 0, sizeof(buf));
        if (!cfs5_write_block(blk, buf))
            return 0;
    }

    for (int e = 0; e < dir.extent_count; e++) {
        cfs5_extent_t* ex = &dir.extents[e];
        for (uint32_t b = 0; b < ex->block_count; b++) {
            uint32_t blk = ex->start_block + b;
            if (!cfs5_read_block(blk, buf))
                return 0;

            cfs5_dirent_t* ents = (cfs5_dirent_t*)buf;
            int max = CFS5_BLOCK_SIZE / sizeof(cfs5_dirent_t);
            for (int i = 0; i < max; i++) {
                if (ents[i].inode &&
                    !cfs5_dirent_valid(&ents[i])) {
                    serial_puts("[fs] ERROR: malformed CFS5 directory entry\n");
                    return 0;
                }
                if (ents[i].inode == 0) {
                    ents[i].inode = child_ino;
                    strncpy(ents[i].name, name, CFS5_MAX_NAME-1);
                    ents[i].name[CFS5_MAX_NAME-1] = 0;
                    if (!cfs5_write_block(blk, buf))
                        return 0;
                    dir.size += sizeof(cfs5_dirent_t);
                    cfs5_write_inode(dir_ino, &dir);
                    return 1;
                }
            }
        }
    }

    if (dir.extent_count >= CFS5_MAX_EXTENTS)
        return 0;

    uint32_t blk;
    if (!cfs5_alloc_block(&blk))
        return 0;

    cfs5_extent_t* nex = &dir.extents[dir.extent_count++];
    nex->start_block = blk;
    nex->block_count = 1;

    memset(buf, 0, sizeof(buf));
    cfs5_dirent_t* ents = (cfs5_dirent_t*)buf;
    ents[0].inode = child_ino;
    strncpy(ents[0].name, name, CFS5_MAX_NAME-1);
    ents[0].name[CFS5_MAX_NAME-1] = 0;

    if (!cfs5_write_block(blk, buf))
        return 0;

    dir.size += sizeof(cfs5_dirent_t);
    cfs5_write_inode(dir_ino, &dir);
    return 1;
}

static int dir_remove_entry(uint32_t dir_ino, const char* name) {
    cfs5_inode_t dir;
    if (!cfs5_name_valid(name)) return 0;
    if (!cfs5_read_inode(dir_ino, &dir))
        return 0;
    if (dir.type != CFS5_INODE_DIR)
        return 0;

    uint8_t buf[CFS5_BLOCK_SIZE];

    for (int e = 0; e < dir.extent_count; e++) {
        cfs5_extent_t* ex = &dir.extents[e];
        for (uint32_t b = 0; b < ex->block_count; b++) {
            uint32_t blk = ex->start_block + b;
            if (!cfs5_read_block(blk, buf))
                return 0;

            cfs5_dirent_t* ents = (cfs5_dirent_t*)buf;
            int max = CFS5_BLOCK_SIZE / sizeof(cfs5_dirent_t);
            for (int i = 0; i < max; i++) {
                if (ents[i].inode == 0)
                    continue;
                if (!cfs5_dirent_valid(&ents[i])) {
                    serial_puts("[fs] ERROR: malformed CFS5 directory entry\n");
                    return 0;
                }
                if (cfs5_dirent_name_equal(&ents[i], name)) {
                    ents[i].inode = 0;
                    ents[i].name[0] = 0;
                    if (!cfs5_write_block(blk, buf))
                        return 0;
                    dir.size -= sizeof(cfs5_dirent_t);
                    cfs5_write_inode(dir_ino, &dir);
                    return 1;
                }
            }
        }
    }

    return 0;
}

static char* kstrtok_r(char* s, const char* delim, char** saveptr) {
    char* token;

    if (s == NULL)
        s = *saveptr;

    while (*s && strchr(delim, *s))
        s++;

    if (*s == '\0') {
        *saveptr = s;
        return NULL;
    }

    token = s;

    while (*s && !strchr(delim, *s))
        s++;

    if (*s) {
        *s = '\0';
        s++;
    }

    *saveptr = s;
    return token;
}

static int path_resolve(const char* path, uint32_t* out_ino) {
    if (!path || !path[0])
        return 0;

    uint32_t cur = g_root_ino;
    if (path[0] == '/')
        path++;

    char tmp[128];
    strncpy(tmp, path, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = 0;

    uint32_t parents[32];
    int depth = 0;
    char* save = NULL;
    char* tok = kstrtok_r(tmp, "/", &save);
    if (!tok) {
        *out_ino = cur;
        return 1;
    }

    while (tok) {
        if (!strcmp(tok, ".")) {
            tok = kstrtok_r(NULL, "/", &save);
            continue;
        }
        if (!strcmp(tok, "..")) {
            if (depth > 0) cur = parents[--depth];
            else cur = g_root_ino;
            tok = kstrtok_r(NULL, "/", &save);
            continue;
        }
        if (depth >= (int)(sizeof(parents) / sizeof(parents[0]))) return 0;
        parents[depth++] = cur;
        if (!dir_find_entry(cur, tok, &cur, NULL, NULL)) return 0;
        tok = kstrtok_r(NULL, "/", &save);
    }

    *out_ino = cur;
    return 1;
}

static int path_parent(const char* path, char* parent, char* name) {
    char tmp[128];
    strncpy(tmp, path, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = 0;

    char* last = strrchr(tmp, '/');
    if (!last) {
        strcpy(parent, "/");
        strncpy(name, tmp, 63);
        name[63] = 0;
        return 1;
    }

    *last = 0;
    strncpy(name, last+1, 63);
    name[63] = 0;

    if (tmp[0] == 0)
        strcpy(parent, "/");
    else
        strncpy(parent, tmp, 127);

    parent[127] = 0;
    return 1;
}


int fs_stat(const char* path, fs_stat_t* st) {
    uint32_t ino;
    cfs5_inode_t in;

    if (vfs_path_type(path) == FS_TYPE_FAT32)
        return vfs_stat(path, st) == 0;
    if (!path_resolve(path, &ino)) {
        return 0;
    }
    if (!cfs5_read_inode(ino, &in)) {
        return 0;
    }

    st->mode = in.mode;
    st->uid  = in.uid;
    st->gid  = in.gid;
    st->size = in.size;
    st->type = in.type;
    return 1;
}


int fs_mkdir(const char* path, uint16_t mode) {
    char parent[128], name[64];
    uint32_t pino, ino;
    cfs5_inode_t in;

    if (vfs_path_type(path) == FS_TYPE_FAT32)
        return vfs_mkdir(path, mode) == 0;
    if (!path_parent(path, parent, name))
        return 0;
    if (!path_resolve(parent, &pino))
        return 0;

    if (dir_find_entry(pino, name, &ino, NULL, NULL))
        return 0;

    if (!cfs5_alloc_inode(&ino))
        return 0;

    memset(&in, 0, sizeof(in));
    in.type = CFS5_INODE_DIR;
    in.mode = mode;
    in.uid  = 0;
    in.gid  = 0;
    in.size = 0;
    in.extent_count = 0;

    cfs5_write_inode(ino, &in);

    if (!dir_add_entry(pino, name, ino))
        return 0;

    return 1;
}

int fs_create(const char* path, uint16_t mode) {
    char parent[128], name[64];
    uint32_t pino, ino;
    cfs5_inode_t in;

    if (vfs_path_type(path) == FS_TYPE_FAT32)
        return vfs_create(path, mode) == 0;
    if (!path_parent(path, parent, name))
        return 0;
    if (!path_resolve(parent, &pino))
        return 0;

    if (dir_find_entry(pino, name, &ino, NULL, NULL))
        return 0;

    if (!cfs5_alloc_inode(&ino))
        return 0;

    memset(&in, 0, sizeof(in));
    in.type = CFS5_INODE_FILE;
    in.mode = mode;
    in.uid  = 0;
    in.gid  = 0;
    in.size = 0;
    in.extent_count = 0;

    cfs5_write_inode(ino, &in);

    if (!dir_add_entry(pino, name, ino))
        return 0;

    return 1;
}

int fs_unlink(const char* path) {
    char parent[128], name[64];
    uint32_t pino, ino;
    cfs5_inode_t in;

    if (vfs_path_type(path) == FS_TYPE_FAT32)
        return vfs_unlink(path) == 0;
    if (!path_parent(path, parent, name))
        return 0;
    if (!path_resolve(parent, &pino))
        return 0;

    if (!dir_find_entry(pino, name, &ino, NULL, NULL))
        return 0;

    if (!cfs5_read_inode(ino, &in))
        return 0;

    for (int e = 0; e < in.extent_count; e++) {
        cfs5_extent_t* ex = &in.extents[e];
        for (uint32_t b = 0; b < ex->block_count; b++) {
            cfs5_free_block(ex->start_block + b);
        }
    }

    in.type = CFS5_INODE_FREE;
    in.size = 0;
    in.extent_count = 0;
    cfs5_write_inode(ino, &in);
    cfs5_free_inode(ino);

    if (!dir_remove_entry(pino, name))
        return 0;

    return 1;
}


static int inode_ensure_space(cfs5_inode_t* in, uint32_t end_offset) {
    uint8_t original_extent_count;
    uint32_t original_last_extent_blocks = 0;
    uint32_t needed_blocks = end_offset / CFS5_BLOCK_SIZE +
        (end_offset % CFS5_BLOCK_SIZE != 0);
    uint64_t have_blocks = 0;

    if (!in || in->extent_count > CFS5_MAX_EXTENTS)
        return 0;
    original_extent_count = in->extent_count;
    if (original_extent_count)
        original_last_extent_blocks =
            in->extents[original_extent_count - 1].block_count;

    for (int e = 0; e < in->extent_count; e++)
        have_blocks += in->extents[e].block_count;

    while (have_blocks < needed_blocks) {
        uint32_t blk;
        if (!cfs5_alloc_block(&blk))
            goto rollback;

        if (in->extent_count &&
            (uint64_t)in->extents[in->extent_count - 1].start_block +
                in->extents[in->extent_count - 1].block_count == blk) {
            in->extents[in->extent_count - 1].block_count++;
        } else {
            if (in->extent_count >= CFS5_MAX_EXTENTS) {
                cfs5_free_block(blk);
                goto rollback;
            }
            cfs5_extent_t* ex = &in->extents[in->extent_count++];
            ex->start_block = blk;
            ex->block_count = 1;
        }
        have_blocks++;
    }

    return 1;

rollback:
    if (original_extent_count &&
        in->extents[original_extent_count - 1].block_count >
            original_last_extent_blocks) {
        cfs5_extent_t* original_last =
            &in->extents[original_extent_count - 1];
        uint32_t added = original_last->block_count -
                         original_last_extent_blocks;
        uint32_t first = original_last->start_block +
                         original_last_extent_blocks;
        for (uint32_t i = 0; i < added; i++)
            cfs5_free_block(first + i);
        original_last->block_count = original_last_extent_blocks;
    }
    while (in->extent_count > original_extent_count) {
        cfs5_extent_t* added = &in->extents[--in->extent_count];
        for (uint32_t i = 0; i < added->block_count; i++)
            cfs5_free_block(added->start_block + i);
        memset(added, 0, sizeof(*added));
    }
    return 0;
}

static int inode_block_for_offset(cfs5_inode_t* in, uint32_t offset, uint32_t* out_block) {
    uint32_t blk_index = offset / CFS5_BLOCK_SIZE;
    uint64_t acc = 0;

    for (int e = 0; e < in->extent_count; e++) {
        cfs5_extent_t* ex = &in->extents[e];
        if (blk_index < acc + ex->block_count) {
            uint32_t local = (uint32_t)(blk_index - acc);
            *out_block = ex->start_block + local;
            return 1;
        }
        acc += ex->block_count;
    }

    return 0;
}


int fs_read(const char* path, uint8_t* buf, uint32_t size, uint32_t offset) {
    uint32_t ino;
    cfs5_inode_t in;

    if (vfs_path_type(path) == FS_TYPE_FAT32)
        return vfs_read(path, buf, size, offset);
    if (!path_resolve(path, &ino))
        return 0;
    if (!cfs5_read_inode(ino, &in))
        return 0;
    if (in.type != CFS5_INODE_FILE)
        return 0;

    if (offset >= in.size)
        return 0;
    if (size > in.size - offset)
        size = in.size - offset;

    uint32_t remaining = size;
    uint32_t pos = offset;
    uint8_t blkbuf[CFS5_BLOCK_SIZE];

    while (remaining > 0) {
        uint32_t blk_off = pos % CFS5_BLOCK_SIZE;
        uint32_t blk;
        if (!inode_block_for_offset(&in, pos, &blk))
            return 0;

        if (!cfs5_read_block(blk, blkbuf))
            return 0;

        uint32_t chunk = CFS5_BLOCK_SIZE - blk_off;
        if (chunk > remaining)
            chunk = remaining;

        memcpy(buf, blkbuf + blk_off, chunk);

        buf += chunk;
        pos += chunk;
        remaining -= chunk;
    }

    return 1;
}

int fs_write(const char* path, const uint8_t* buf, uint32_t size, uint32_t offset) {
    uint32_t ino;
    cfs5_inode_t in;

    if (vfs_path_type(path) == FS_TYPE_FAT32)
        return vfs_write(path, buf, size, offset);
    if (!path_resolve(path, &ino))
        return 0;
    if (!cfs5_read_inode(ino, &in))
        return 0;
    if (in.type != CFS5_INODE_FILE)
        return 0;

    if (size > UINT32_MAX - offset)
        return 0;
    uint32_t end = offset + size;
    if (!inode_ensure_space(&in, end))
        return 0;

    uint32_t remaining = size;
    uint32_t pos = offset;
    uint8_t blkbuf[CFS5_BLOCK_SIZE];

    while (remaining > 0) {
        uint32_t blk_off = pos % CFS5_BLOCK_SIZE;
        uint32_t blk;
        if (!inode_block_for_offset(&in, pos, &blk))
            return 0;

        if (!cfs5_read_block(blk, blkbuf))
            memset(blkbuf, 0, sizeof(blkbuf));

        uint32_t chunk = CFS5_BLOCK_SIZE - blk_off;
        if (chunk > remaining)
            chunk = remaining;

        memcpy(blkbuf + blk_off, buf, chunk);

        if (!cfs5_write_block(blk, blkbuf))
            return 0;

        buf += chunk;
        pos += chunk;
        remaining -= chunk;
    }

    if (end > in.size)
        in.size = end;

    return cfs5_write_inode(ino, &in);
}


int fs_list(const char* path, char names[][CFS5_MAX_NAME], int max) {
    if (vfs_path_type(path) == FS_TYPE_FAT32)
        return vfs_list(path, names, max);
    return fs_list_offset(path, names, 0, max);
}

int fs_list_offset(const char* path, char names[][CFS5_MAX_NAME],
                   uint32_t offset, int max) {
    uint32_t ino;
    cfs5_inode_t dir;

    if (vfs_path_type(path) == FS_TYPE_FAT32) {
        char entries[128][CFS5_MAX_NAME];
        uint64_t requested;
        int count;
        int copied = 0;
        if (!names || max <= 0 || offset >= 128) return 0;
        requested = (uint64_t)offset + (uint32_t)max;
        if (requested > 128) requested = 128;
        count = vfs_list(path, entries, (int)requested);
        if (count < 0) return 0;
        for (uint32_t i = offset; i < (uint32_t)count && copied < max; i++)
            strcpy(names[copied++], entries[i]);
        return copied;
    }
    if (!path_resolve(path, &ino) || !names || max <= 0)
        return 0;
    if (!cfs5_read_inode(ino, &dir))
        return 0;
    if (dir.type != CFS5_INODE_DIR)
        return 0;
    if (dir.size % sizeof(cfs5_dirent_t) != 0) {
        serial_puts("[fs] ERROR: malformed CFS5 directory size\n");
        return -1;
    }
    uint32_t entries_remaining =
        dir.size / (uint32_t)sizeof(cfs5_dirent_t);
    if (!entries_remaining) return 0;

    int count = 0;
    uint8_t buf[CFS5_BLOCK_SIZE];

    for (int e = 0; e < dir.extent_count; e++) {
        cfs5_extent_t* ex = &dir.extents[e];
        for (uint32_t b = 0; b < ex->block_count; b++) {
            uint32_t blk = ex->start_block + b;
            if (!cfs5_read_block(blk, buf))
                return count;

            cfs5_dirent_t* ents = (cfs5_dirent_t*)buf;
            int maxent = CFS5_BLOCK_SIZE / sizeof(cfs5_dirent_t);
            for (int i = 0; i < maxent; i++) {
                if (ents[i].inode == 0)
                    continue;
                if (!cfs5_dirent_valid(&ents[i])) {
                    serial_puts("[fs] ERROR: malformed CFS5 directory entry\n");
                    return -1;
                }
                if (!entries_remaining) {
                    serial_puts("[fs] ERROR: directory has more entries than its size\n");
                    return -1;
                }
                entries_remaining--;
                if (offset) {
                    offset--;
                    continue;
                }
                if (count >= max)
                    return count;
                strncpy(names[count], ents[i].name, CFS5_MAX_NAME-1);
                names[count][CFS5_MAX_NAME-1] = 0;
                count++;
                if (count >= max || !entries_remaining)
                    return count;
            }
        }
    }

    if (entries_remaining) {
        serial_puts("[fs] ERROR: directory size exceeds its entries\n");
        return -1;
    }
    return count;
}


int fs_chmod(const char* path, uint16_t mode) {
    uint32_t ino;
    cfs5_inode_t in;

    if (!path_resolve(path, &ino))
        return 0;
    if (!cfs5_read_inode(ino, &in))
        return 0;

    in.mode = mode;
    return cfs5_write_inode(ino, &in);
}

int fs_chown(const char* path, uint16_t uid, uint16_t gid) {
    uint32_t ino;
    cfs5_inode_t in;

    if (!path_resolve(path, &ino))
        return 0;
    if (!cfs5_read_inode(ino, &in))
        return 0;

    in.uid = uid;
    in.gid = gid;
    return cfs5_write_inode(ino, &in);
}

int fs_check_access(const char* path, uint16_t uid, uint16_t gid, int want) {
    uint32_t ino;
    cfs5_inode_t in;

    if (vfs_path_type(path) == FS_TYPE_FAT32) {
        fs_stat_t status;
        unsigned int shift;
        if (!fs_stat(path, &status)) return 0;
        if (uid == 0) return 1;
        shift = uid == status.uid ? 6U : gid == status.gid ? 3U : 0U;
        return ((((status.mode >> shift) & 7U) & (unsigned int)want) ==
                (unsigned int)want);
    }
    if (!path_resolve(path, &ino))
        return 0;
    if (!cfs5_read_inode(ino, &in))
        return 0;

    if (uid == 0)
        return 1;

    uint16_t r_bit, w_bit, x_bit;

    if (uid == in.uid) {
        r_bit = CFS5_S_IRUSR;
        w_bit = CFS5_S_IWUSR;
        x_bit = CFS5_S_IXUSR;
    } else if (gid == in.gid) {
        r_bit = CFS5_S_IRGRP;
        w_bit = CFS5_S_IWGRP;
        x_bit = CFS5_S_IXGRP;
    } else {
        r_bit = CFS5_S_IROTH;
        w_bit = CFS5_S_IWOTH;
        x_bit = CFS5_S_IXOTH;
    }

    int ok = 0;
    if (want & FS_PERM_READ)
        ok |= (in.mode & r_bit) ? FS_PERM_READ : 0;
    if (want & FS_PERM_WRITE)
        ok |= (in.mode & w_bit) ? FS_PERM_WRITE : 0;
    if (want & FS_PERM_EXEC)
        ok |= (in.mode & x_bit) ? FS_PERM_EXEC : 0;

    return (ok & want) == want;
}


int fs_read_block(uint32_t lba, void* buf) {
    return cfs5_read_block(lba, buf);
}

uint32_t fs_total_space(void) {
    cfs5_superblock_t* sb = cfs5_get_super();
    if (!sb) return 0;
    return sb->block_count * CFS5_BLOCK_SIZE;
}

uint32_t fs_used_space(void) {
    cfs5_superblock_t* sb = cfs5_get_super();
    uint32_t free_blocks = 0;
    uint8_t bitmap[CFS5_BLOCK_SIZE];
    if (!sb) return 0;
    for (uint32_t block = 0; block < sb->free_block_bitmap_blocks; block++) {
        if (!cfs5_read_block(sb->free_block_bitmap + block, bitmap)) return 0;
        for (uint32_t byte = 0; byte < CFS5_BLOCK_SIZE; byte++) {
            uint8_t value = bitmap[byte];
            for (uint32_t bit = 0; bit < 8; bit++) {
                uint32_t index = block * CFS5_BLOCK_SIZE * 8 + byte * 8 + bit;
                if (index < sb->block_count && !(value & (1U << bit)))
                    free_blocks++;
            }
        }
    }
    return sb->block_count - free_blocks;
}

int fs_load_file(const char* path, uint8_t* buf, uint32_t max, uint32_t* out_size) {
    fs_stat_t st;
    if (!fs_stat(path, &st) || st.type != CFS5_INODE_FILE)
        return 0;

    uint32_t to_read = st.size;
    if (to_read > max)
        to_read = max;

    if (!fs_read(path, buf, to_read, 0))
        return 0;

    if (out_size)
        *out_size = to_read;
    return 1;
}

int fs_write_file(const char* path, const uint8_t* buf, uint32_t size) {
    fs_stat_t st;
    if (vfs_path_type(path) == FS_TYPE_FAT32) {
        if (!fs_stat(path, &st)) {
            if (!fs_create(path, 0644))
                return 0;
        } else if (st.type != FS_TYPE_FILE) {
            return 0;
        }
        if (vfs_truncate(path, 0) != 1) return 0;
        return !size || fs_write(path, buf, size, 0) == (int)size;
    }
    if (!fs_stat(path, &st)) {
        if (!fs_create(path, 0644))
            return 0;
    }
    if (!fs_write(path, buf, size, 0))
        return 0;

    uint32_t ino;
    cfs5_inode_t inode;
    if (!path_resolve(path, &ino) || !cfs5_read_inode(ino, &inode))
        return 0;
    inode.size = size;
    return cfs5_write_inode(ino, &inode);
}

int fs_exists(const char* path) {
    fs_stat_t st;
    return fs_stat(path, &st);
}

int fs_copy(const char* src, const char* dst) {
    fs_stat_t st;
    fs_stat_t copied;
    uint8_t verify[512];
    if (!fs_stat(src, &st) || st.type != CFS5_INODE_FILE)
        return 0;
    if (!strcmp(src, dst) || st.size > UINT32_MAX)
        return 0;

    uint8_t buf[512];
    uint32_t off = 0;

    if (!fs_create(dst, st.mode))
        return 0;

    while (off < st.size) {
        uint32_t chunk = (st.size - off > sizeof(buf)) ? sizeof(buf) : (st.size - off);
        if (!fs_read(src, buf, chunk, off))
            goto fail;
        if (!fs_write(dst, buf, chunk, off))
            goto fail;
        if (!fs_read(dst, verify, chunk, off) || memcmp(buf, verify, chunk) != 0)
            goto fail;
        off += chunk;
    }
    if (!fs_stat(dst, &copied) || copied.type != CFS5_INODE_FILE ||
        copied.size != st.size)
        goto fail;
    return 1;

fail:
    fs_unlink(dst);
    return 0;
}

int fs_rename(const char* src, const char* dst) {
    if (!fs_copy(src, dst))
        return 0;
    return fs_unlink(src);
}

int fs_chdir(const char* path) {
    fs_stat_t st;
    if (!fs_stat(path, &st))
        return 0;
    if (st.type != FS_TYPE_DIR)
        return 0;
    return 1;
}

int fs_get_statfs(const char* path, statfs_t* buf) {
    (void)path;
    cfs5_superblock_t* sb = cfs5_get_super();
    if (!sb) return -1;

    buf->f_type = 0x20250729;  /* CFS5 magic */
    buf->f_bsize = CFS5_BLOCK_SIZE;
    buf->f_blocks = sb->block_count;
    uint64_t free_blocks = 0;
    uint64_t free_inodes = 0;
    uint8_t bitmap[CFS5_BLOCK_SIZE];
    for (uint32_t block = 0; block < sb->free_block_bitmap_blocks; block++) {
        if (!cfs5_read_block(sb->free_block_bitmap + block, bitmap)) return -1;
        for (uint32_t byte = 0; byte < CFS5_BLOCK_SIZE; byte++) {
            for (uint32_t bit = 0; bit < 8; bit++) {
                uint32_t index = block * CFS5_BLOCK_SIZE * 8 + byte * 8 + bit;
                if (index < sb->block_count && !(bitmap[byte] & (1U << bit)))
                    free_blocks++;
            }
        }
    }
    for (uint32_t block = 0; block < sb->free_inode_bitmap_blocks; block++) {
        if (!cfs5_read_block(sb->free_inode_bitmap + block, bitmap)) return -1;
        for (uint32_t byte = 0; byte < CFS5_BLOCK_SIZE; byte++) {
            for (uint32_t bit = 0; bit < 8; bit++) {
                uint32_t index = block * CFS5_BLOCK_SIZE * 8 + byte * 8 + bit;
                if (index < sb->inode_count && !(bitmap[byte] & (1U << bit)))
                    free_inodes++;
            }
        }
    }
    buf->f_bfree = free_blocks;
    buf->f_bavail = free_blocks;
    buf->f_files = sb->inode_count;
    buf->f_ffree = free_inodes;
    buf->f_fsid = 0;
    buf->f_namelen = CFS5_MAX_NAME - 1;
    return 0;
}

int fs_unmount(const char* path) {
    return vfs_unmount(path);
}
