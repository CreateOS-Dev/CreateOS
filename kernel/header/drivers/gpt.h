// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef GPT_H
#define GPT_H

#include <stdint.h>

#define GPT_MAX_PARTITIONS 128

typedef struct {
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint8_t type_guid[16];
    uint8_t partition_guid[16];
    char name[37];
} gpt_partition_t;

int gpt_init(void);
int gpt_available(void);
int gpt_partition_count(void);
const gpt_partition_t* gpt_partition_at(int index);

#endif