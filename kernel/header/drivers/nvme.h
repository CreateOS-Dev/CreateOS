// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef NVME_H
#define NVME_H

#include <stdint.h>

int nvme_init(void);
int nvme_available(void);
uint32_t nvme_namespace_count(void);
uint32_t nvme_namespace_block_size(void);
uint64_t nvme_namespace_block_count(void);
int nvme_read_block(uint64_t lba, void* buffer);
int nvme_write_block(uint64_t lba, const void* buffer);
int nvme_flush(void);
const char* nvme_serial(void);
const char* nvme_model(void);

#endif