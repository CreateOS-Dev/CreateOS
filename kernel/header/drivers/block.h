// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>

#define BLOCK_SECTOR_SIZE 512

typedef struct {
    int (*read)(void* context, uint64_t lba, uint8_t* buffer);
    int (*write)(void* context, uint64_t lba, const uint8_t* buffer);
    int (*flush)(void* context);
} block_device_ops_t;

int block_read(uint64_t lba, uint8_t* buffer);
int block_write(uint64_t lba, const uint8_t* buffer);
int block_flush(void);
int block_register_device(const char* name, uint64_t sector_count,
                          const block_device_ops_t* ops, void* context);
int block_unregister_device(const char* name);
int block_device_sector_count(const char* name, uint64_t* sector_count);
int block_read_device(const char* name, uint64_t lba, uint8_t* buffer);
int block_write_device(const char* name, uint64_t lba, const uint8_t* buffer);
int block_flush_device(const char* name);

#endif