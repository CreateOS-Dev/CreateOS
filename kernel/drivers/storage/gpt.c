// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "drivers/gpt.h"
#include "drivers/block.h"
#include <stddef.h>

#define GPT_SIGNATURE "EFI PART"
#define GPT_HEADER_LBA 1
#define GPT_HEADER_MIN_SIZE 92
#define GPT_ENTRY_MIN_SIZE 128
#define GPT_ENTRY_MAX_SIZE 4096

static gpt_partition_t partitions[GPT_MAX_PARTITIONS];
static int partition_count;
static int table_available;

static uint32_t crc32(const uint8_t* data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

static int is_zero_guid(const uint8_t* guid)
{
    for (int i = 0; i < 16; i++)
        if (guid[i] != 0) return 0;
    return 1;
}

int gpt_init(void)
{
    uint8_t sector[BLOCK_SECTOR_SIZE];
    uint8_t header_copy[BLOCK_SECTOR_SIZE];
    uint64_t entries_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint32_t header_size;
    uint32_t header_crc;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t table_sectors;
    uint32_t table_crc;

    partition_count = 0;
    table_available = 0;
    if (!block_read(GPT_HEADER_LBA, sector)) return 0;
    for (int i = 0; i < 8; i++)
        if (sector[i] != (uint8_t)GPT_SIGNATURE[i]) return 0;

    header_size = *(uint32_t*)(sector + 12);
    header_crc = *(uint32_t*)(sector + 16);
    if (header_size < GPT_HEADER_MIN_SIZE || header_size > BLOCK_SECTOR_SIZE)
        return 0;
    for (uint32_t i = 0; i < header_size; i++) header_copy[i] = sector[i];
    *(uint32_t*)(header_copy + 16) = 0;
    if (crc32(header_copy, header_size) != header_crc) return 0;

    first_usable = *(uint64_t*)(sector + 40);
    last_usable = *(uint64_t*)(sector + 48);
    entries_lba = *(uint64_t*)(sector + 72);
    entry_count = *(uint32_t*)(sector + 80);
    entry_size = *(uint32_t*)(sector + 84);
    table_crc = *(uint32_t*)(sector + 88);
    if (first_usable > last_usable || !entry_count ||
        entry_size < GPT_ENTRY_MIN_SIZE || entry_size > GPT_ENTRY_MAX_SIZE ||
        entries_lba > 0x0FFFFFFFULL) return 0;
    if (entry_count > GPT_MAX_PARTITIONS) entry_count = GPT_MAX_PARTITIONS;
    table_sectors = (entry_count * entry_size + BLOCK_SECTOR_SIZE - 1) /
                    BLOCK_SECTOR_SIZE;
    if (entries_lba + table_sectors - 1 > 0x0FFFFFFFULL) return 0;

    uint32_t calculated_table_crc = 0xFFFFFFFFU;
    uint32_t table_bytes = entry_count * entry_size;
    for (uint32_t table_sector = 0; table_sector < table_sectors; table_sector++) {
        uint8_t table_data[BLOCK_SECTOR_SIZE];
        if (!block_read(entries_lba + table_sector, table_data)) return 0;
        uint32_t bytes = table_bytes > BLOCK_SECTOR_SIZE ? BLOCK_SECTOR_SIZE : table_bytes;
        for (uint32_t byte = 0; byte < bytes; byte++) {
            calculated_table_crc ^= table_data[byte];
            for (int bit = 0; bit < 8; bit++)
                calculated_table_crc = (calculated_table_crc >> 1) ^
                    (0xEDB88320U & (uint32_t)-(int32_t)(calculated_table_crc & 1));
        }
        table_bytes -= bytes;
    }
    if (~calculated_table_crc != table_crc) return 0;

    for (uint32_t index = 0; index < entry_count; index++) {
        uint32_t offset = index * entry_size;
        uint64_t lba = entries_lba + offset / BLOCK_SECTOR_SIZE;
        uint32_t in_sector = offset % BLOCK_SECTOR_SIZE;
        uint8_t entry[GPT_ENTRY_MAX_SIZE];
        uint32_t remaining = entry_size;
        uint32_t copied = 0;

        while (remaining) {
            uint8_t current[BLOCK_SECTOR_SIZE];
            uint32_t chunk = BLOCK_SECTOR_SIZE - in_sector;
            if (chunk > remaining) chunk = remaining;
            if (!block_read(lba, current)) return 0;
            for (uint32_t byte = 0; byte < chunk; byte++)
                entry[copied + byte] = current[in_sector + byte];
            copied += chunk;
            remaining -= chunk;
            lba++;
            in_sector = 0;
        }
        if (is_zero_guid(entry)) continue;
        uint64_t first = *(uint64_t*)(entry + 32);
        uint64_t last = *(uint64_t*)(entry + 40);
        if (first > last || first < first_usable || last > last_usable) continue;
        gpt_partition_t* partition = &partitions[partition_count];
        for (int byte = 0; byte < 16; byte++) {
            partition->type_guid[byte] = entry[byte];
            partition->partition_guid[byte] = entry[16 + byte];
        }
        partition->first_lba = first;
        partition->last_lba = last;
        partition->attributes = *(uint64_t*)(entry + 48);
        for (int character = 0; character < 36; character++) {
            uint16_t value = *(uint16_t*)(entry + 56 + character * 2);
            partition->name[character] = value < 128 ? (char)value : '?';
            if (!value) {
                partition->name[character] = 0;
                break;
            }
        }
        partition->name[36] = 0;
        partition_count++;
    }
    table_available = 1;
    return 1;
}

int gpt_available(void) { return table_available; }
int gpt_partition_count(void) { return partition_count; }

const gpt_partition_t* gpt_partition_at(int index)
{
    if (index < 0 || index >= partition_count) return 0;
    return &partitions[index];
}