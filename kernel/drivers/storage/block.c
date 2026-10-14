// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "drivers/block.h"
#include "drivers/ata.h"
#include "drivers/nvme.h"
#include "memory/pmm.h"
#include "lib/string.h"
#include <stddef.h>

#define ATA_LBA28_MAX 0x0FFFFFFFULL
#define BLOCK_MAX_REGISTERED_DEVICES 72
#define BLOCK_DEVICE_NAME_MAX 64

typedef struct {
    char name[BLOCK_DEVICE_NAME_MAX];
    uint64_t sector_count;
    block_device_ops_t ops;
    void* context;
    int used;
} registered_block_device_t;

static registered_block_device_t registered_devices[
    BLOCK_MAX_REGISTERED_DEVICES];

static registered_block_device_t* block_find_registered(const char* name)
{
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < BLOCK_MAX_REGISTERED_DEVICES; i++) {
        if (registered_devices[i].used &&
            strcmp(registered_devices[i].name, name) == 0)
            return &registered_devices[i];
    }
    return NULL;
}

int block_register_device(const char* name, uint64_t sector_count,
                          const block_device_ops_t* ops, void* context)
{
    registered_block_device_t* slot = NULL;
    size_t name_length;

    if (!name || !ops || !ops->read || !ops->write || !ops->flush ||
        !sector_count)
        return 0;
    name_length = strlen(name);
    if (!name_length || name_length >= BLOCK_DEVICE_NAME_MAX ||
        block_find_registered(name))
        return 0;
    for (int i = 0; i < BLOCK_MAX_REGISTERED_DEVICES; i++) {
        if (!registered_devices[i].used) {
            slot = &registered_devices[i];
            break;
        }
    }
    if (!slot) return 0;

    memset(slot, 0, sizeof(*slot));
    memcpy(slot->name, name, name_length + 1);
    slot->sector_count = sector_count;
    slot->ops = *ops;
    slot->context = context;
    slot->used = 1;
    return 1;
}

int block_unregister_device(const char* name)
{
    registered_block_device_t* device = block_find_registered(name);
    if (!device) return 0;
    memset(device, 0, sizeof(*device));
    return 1;
}

int block_device_sector_count(const char* name, uint64_t* sector_count)
{
    registered_block_device_t* device = block_find_registered(name);
    if (!device || !sector_count) return 0;
    *sector_count = device->sector_count;
    return 1;
}

int block_read_device(const char* name, uint64_t lba, uint8_t* buffer)
{
    registered_block_device_t* device = block_find_registered(name);
    if (!name || !buffer) return 0;
    if (device) {
        if (lba >= device->sector_count) return 0;
        return device->ops.read(device->context, lba, buffer);
    }
    if (strcmp(name, "ata0-master") == 0 ||
        strcmp(name, "default") == 0)
        return block_read(lba, buffer);
    return 0;
}

int block_write_device(const char* name, uint64_t lba, const uint8_t* buffer)
{
    registered_block_device_t* device = block_find_registered(name);
    if (!name || !buffer) return 0;
    if (device) {
        if (lba >= device->sector_count) return 0;
        return device->ops.write(device->context, lba, buffer);
    }
    if (strcmp(name, "ata0-master") == 0 ||
        strcmp(name, "default") == 0)
        return block_write(lba, buffer);
    return 0;
}

int block_flush_device(const char* name)
{
    registered_block_device_t* device = block_find_registered(name);
    if (device) return device->ops.flush(device->context);
    if (name && (strcmp(name, "ata0-master") == 0 ||
                 strcmp(name, "default") == 0))
        return block_flush();
    return 0;
}

static int block_nvme_transfer(uint64_t lba, uint8_t* buffer, int write)
{
    uint8_t* aligned;
    int result;
    if (!buffer || !nvme_available() || nvme_namespace_count() == 0 ||
        nvme_namespace_block_size() != BLOCK_SECTOR_SIZE ||
        lba >= nvme_namespace_block_count())
        return 0;
    aligned = (uint8_t*)pmm_alloc_frame();
    if (!aligned) return 0;
    if (write) {
        memcpy(aligned, buffer, BLOCK_SECTOR_SIZE);
        result = nvme_write_block(lba, aligned);
    } else {
        result = nvme_read_block(lba, aligned);
        if (result) memcpy(buffer, aligned, BLOCK_SECTOR_SIZE);
    }
    pmm_free_frame(aligned);
    return result;
}

int block_read(uint64_t lba, uint8_t* buffer)
{
    if (!buffer) return 0;
    if (ata_block_available())
        return lba <= ATA_LBA28_MAX && ata_read28((uint32_t)lba, buffer);
    return block_nvme_transfer(lba, buffer, 0);
}

int block_write(uint64_t lba, const uint8_t* buffer)
{
    if (!buffer) return 0;
    if (ata_block_available())
        return lba <= ATA_LBA28_MAX && ata_write28((uint32_t)lba, buffer);
    return block_nvme_transfer(lba, (uint8_t*)buffer, 1);
}

int block_flush(void)
{
    if (ata_block_available()) return ata_flush();
    if (nvme_available()) return nvme_flush();
    return 0;
}