// ============================================================
// CreateOS 0.0.1
// ============================================================


#ifndef ATA_H
#define ATA_H

#include <stdint.h>

int ata_detect(void);
int ata_block_available(void);
int ata_flush(void);
int ata_read28(uint32_t lba, uint8_t* buffer);
int ata_write28(uint32_t lba, const uint8_t* buffer);
int ata_is_atapi(void);
int atapi_read_sector(uint32_t lba, uint8_t* buffer);

#endif