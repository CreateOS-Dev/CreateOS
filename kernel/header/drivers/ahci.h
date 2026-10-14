// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

int ahci_init(void);
int ahci_available(void);
int ahci_read28(uint32_t lba, uint8_t* buffer);
int ahci_write28(uint32_t lba, const uint8_t* buffer);
int ahci_flush(void);

#endif