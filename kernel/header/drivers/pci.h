// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef PCI_H
#define PCI_H

#include <stdint.h>

typedef struct {
    uint64_t base;
    uint64_t size;
    uint8_t type;
    uint8_t prefetchable;
    int present;
} pci_bar_t;

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor;
    uint16_t device;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint32_t bar[6];
    pci_bar_t bars[6];
} pci_device_t;

typedef struct {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint16_t vendor;
    uint16_t device;
} pci_info_t;

int pci_find_class(uint8_t class_code, uint8_t subclass, pci_device_t* out);
uint32_t pci_read_config32(const pci_device_t* dev, uint8_t offset);
void pci_write_config32(const pci_device_t* dev, uint8_t offset, uint32_t value);
int pci_enable_device(const pci_device_t* dev, int bus_master);
int pci_scan_devices(void);
int pci_device_count(void);
const pci_device_t* pci_device_at(int index);
const pci_bar_t* pci_bar_info(const pci_device_t* dev, int index);
void* pci_map_bar(const pci_device_t* dev, int index);

#endif
