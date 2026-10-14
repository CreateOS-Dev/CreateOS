// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "drivers/pci.h"
#include "drivers/acpi.h"
#include "arch/x86/io.h"
#include "memory/vmm.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define PCI_VENDOR_NONE    0xFFFF
#define PCI_MAX_DEVICES    64

static pci_device_t devices[PCI_MAX_DEVICES];
static int device_count;
static int scan_complete;

static uint32_t pci_config_address(uint8_t bus, uint8_t slot,
                                   uint8_t function, uint8_t offset)
{
    return 0x80000000U | ((uint32_t)bus << 16) |
           ((uint32_t)slot << 11) | ((uint32_t)function << 8) |
           (offset & 0xFC);
}

static volatile uint32_t* pci_ecam_address(uint8_t bus, uint8_t slot,
                                            uint8_t function, uint8_t offset)
{
    uint64_t base = acpi_mcfg_base();
    uint64_t address = base + ((uint64_t)(bus - acpi_mcfg_start_bus()) << 20) +
                       ((uint64_t)slot << 15) + ((uint64_t)function << 12) +
                       (offset & 0xFFC);
    return (volatile uint32_t*)(uintptr_t)address;
}

uint32_t pci_read_config32(const pci_device_t* dev, uint8_t offset)
{
    if (!dev || offset > 0xFC || (offset & 3)) return 0xFFFFFFFFU;
    if (acpi_mcfg_available() && dev->bus >= acpi_mcfg_start_bus() &&
        dev->bus <= acpi_mcfg_end_bus())
        return *pci_ecam_address(dev->bus, dev->slot, dev->function, offset);
    outl(PCI_CONFIG_ADDRESS, pci_config_address(
        dev->bus, dev->slot, dev->function, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config32(const pci_device_t* dev, uint8_t offset, uint32_t value)
{
    if (!dev || offset > 0xFC || (offset & 3)) return;
    if (acpi_mcfg_available() && dev->bus >= acpi_mcfg_start_bus() &&
        dev->bus <= acpi_mcfg_end_bus()) {
        *pci_ecam_address(dev->bus, dev->slot, dev->function, offset) = value;
        return;
    }
    outl(PCI_CONFIG_ADDRESS, pci_config_address(
        dev->bus, dev->slot, dev->function, offset));
    outl(PCI_CONFIG_DATA, value);
}

int pci_enable_device(const pci_device_t* dev, int bus_master)
{
    uint32_t command;
    if (!dev) return 0;
    command = pci_read_config32(dev, 0x04);
    if (command == 0xFFFFFFFFU) return 0;
    command |= 0x00000003U; /* I/O space and memory space */
    if (bus_master) command |= 0x00000004U;
    pci_write_config32(dev, 0x04, command);
    return (pci_read_config32(dev, 0x04) & 0x7U) == (command & 0x7U);
}

static uint32_t pci_read_raw(uint8_t bus, uint8_t slot, uint8_t function,
                             uint8_t offset)
{
    if (acpi_mcfg_available() && bus >= acpi_mcfg_start_bus() &&
        bus <= acpi_mcfg_end_bus())
        return *pci_ecam_address(bus, slot, function, offset);
    uint32_t address = pci_config_address(bus, slot, function, offset);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void pci_probe_bars(pci_device_t* dev)
{
    uint8_t header_type;
    uint32_t original_command;

    header_type = (uint8_t)(pci_read_config32(dev, 0x0C) >> 16);
    if ((header_type & 0x7F) == 0x01) return;

    original_command = pci_read_config32(dev, 0x04);
    if (original_command == 0xFFFFFFFFU) return;
    pci_write_config32(dev, 0x04, (original_command & 0xFFFFU) & ~0x7U);

    for (uint8_t i = 0; i < 6; i++) {
        pci_bar_t* bar = &dev->bars[i];
        uint8_t offset = (uint8_t)(0x10 + i * 4);
        uint32_t original = dev->bar[i];
        uint32_t measured;

        bar->base = 0;
        bar->size = 0;
        bar->type = 0;
        bar->prefetchable = 0;
        bar->present = 0;
        if (!original || original == 0xFFFFFFFFU) continue;

        if (original & 1) {
            pci_write_config32(dev, offset, 0xFFFFFFFFU);
            measured = pci_read_config32(dev, offset);
            pci_write_config32(dev, offset, original);
            bar->base = original & ~0x3ULL;
            bar->size = (uint64_t)(~(measured & ~0x3U) + 1U);
            bar->type = 1;
        } else {
            uint8_t memory_type = (uint8_t)((original >> 1) & 0x3);
            uint32_t original_high = 0;
            uint32_t measured_high = 0;
            uint64_t mask;

            bar->prefetchable = (uint8_t)((original >> 3) & 1);
            if (memory_type == 1 || memory_type == 3 ||
                (memory_type == 2 && i >= 5))
                continue;
            if (memory_type == 2 && i < 5) {
                original_high = pci_read_config32(dev, (uint8_t)(offset + 4));
                pci_write_config32(dev, offset, 0xFFFFFFFFU);
                pci_write_config32(dev, (uint8_t)(offset + 4), 0xFFFFFFFFU);
                measured = pci_read_config32(dev, offset);
                measured_high = pci_read_config32(dev, (uint8_t)(offset + 4));
                pci_write_config32(dev, offset, original);
                pci_write_config32(dev, (uint8_t)(offset + 4), original_high);
                bar->base = ((uint64_t)original_high << 32) | (original & ~0xFULL);
                mask = ((uint64_t)measured_high << 32) | (measured & ~0xFULL);
                bar->size = ~mask + 1;
                bar->type = 2;
                dev->bars[i + 1].present = 0;
                i++;
            } else {
                pci_write_config32(dev, offset, 0xFFFFFFFFU);
                measured = pci_read_config32(dev, offset);
                pci_write_config32(dev, offset, original);
                bar->base = original & ~0xFULL;
                bar->size = (uint32_t)(~(measured & ~0x0FU) + 1U);
                bar->type = 0;
            }
        }
        bar->present = bar->size != 0;
    }
    pci_write_config32(dev, 0x04, original_command & 0xFFFFU);
}

int pci_scan_devices(void)
{
    if (scan_complete) return device_count;
    device_count = 0;
    for (uint16_t bus = 0; bus < 256 && device_count < PCI_MAX_DEVICES; bus++) {
        for (uint8_t slot = 0; slot < 32 && device_count < PCI_MAX_DEVICES; slot++) {
            for (uint8_t function = 0; function < 8 && device_count < PCI_MAX_DEVICES; function++) {
                uint32_t id = pci_read_raw((uint8_t)bus, slot, function, 0);
                if ((uint16_t)id == PCI_VENDOR_NONE) continue;
                uint32_t class_info = pci_read_raw((uint8_t)bus, slot, function, 8);
                pci_device_t* dev = &devices[device_count++];
                dev->bus = (uint8_t)bus;
                dev->slot = slot;
                dev->function = function;
                dev->vendor = (uint16_t)id;
                dev->device = (uint16_t)(id >> 16);
                dev->class_code = (uint8_t)(class_info >> 24);
                dev->subclass = (uint8_t)(class_info >> 16);
                dev->prog_if = (uint8_t)(class_info >> 8);
                for (uint8_t i = 0; i < 6; i++)
                    dev->bar[i] = pci_read_raw((uint8_t)bus, slot, function,
                                                (uint8_t)(0x10 + i * 4));
                pci_probe_bars(dev);
            }
        }
    }
    scan_complete = 1;
    return device_count;
}

int pci_device_count(void)
{
    return pci_scan_devices();
}

const pci_device_t* pci_device_at(int index)
{
    if (index < 0 || index >= pci_scan_devices()) return 0;
    return &devices[index];
}

const pci_bar_t* pci_bar_info(const pci_device_t* dev, int index)
{
    if (!dev || index < 0 || index >= 6) return 0;
    return &dev->bars[index];
}

void* pci_map_bar(const pci_device_t* dev, int index)
{
    const pci_bar_t* bar = pci_bar_info(dev, index);

    if (!bar || !bar->present ||
        (bar->type != 0 && bar->type != 2) || !bar->base || !bar->size)
        return 0;
    if (bar->size > UINT64_MAX - bar->base)
        return 0;
    return vmm_map_mmio_range(bar->base, bar->size);
}

int pci_find_class(uint8_t class_code, uint8_t subclass, pci_device_t* out)
{
    if (!out) return 0;
    for (int i = 0; i < pci_scan_devices(); i++)
        if (devices[i].class_code == class_code && devices[i].subclass == subclass) {
            *out = devices[i];
            return 1;
        }
    return 0;
}
