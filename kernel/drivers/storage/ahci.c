// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "drivers/ahci.h"
#include "drivers/pci.h"
#include <stdint.h>

#define AHCI_CLASS 0x01
#define AHCI_SUBCLASS 0x06
#define AHCI_PROGIF 0x01
#define AHCI_PORT_COUNT 32
#define AHCI_PORT_BASE 0x100
#define AHCI_PORT_SIZE 0x80
#define AHCI_PORT_CMD 0x18
#define AHCI_PORT_TFD 0x20
#define AHCI_PORT_SIG 0x24
#define AHCI_PORT_SSTS 0x28
#define AHCI_PORT_SERR 0x30
#define AHCI_PORT_CI 0x38
#define AHCI_GHC 0x04
#define AHCI_CAP 0x00
#define AHCI_GHC_AE (1U << 31)
#define AHCI_GHC_RESET (1U << 0)
#define AHCI_CMD_ST (1U << 0)
#define AHCI_CMD_FRE (1U << 4)
#define AHCI_CMD_FR (1U << 14)
#define AHCI_CMD_CR (1U << 15)
#define AHCI_TFD_BSY 0x80
#define AHCI_TFD_DRQ 0x08
#define AHCI_SIG_SATA 0x00000101U

typedef struct {
    uint32_t opts;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} ahci_command_header_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved2;
    uint32_t dbc;
} ahci_command_table_t;

static volatile uint8_t* abar;
static volatile uint8_t* port;
static ahci_command_header_t command_list[32] __attribute__((aligned(1024)));
static ahci_command_table_t command_table __attribute__((aligned(128)));
static int initialized;

static uint32_t mmio_read32(uint32_t offset)
{
    return *(volatile uint32_t*)(abar + offset);
}

static void mmio_write32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t*)(abar + offset) = value;
}

static uint32_t port_read32(uint32_t offset)
{
    return *(volatile uint32_t*)(port + offset);
}

static void port_write32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t*)(port + offset) = value;
}

static int wait_port_idle(void)
{
    for (uint32_t i = 0; i < 100000; i++) {
        uint32_t command = port_read32(AHCI_PORT_CMD);
        if (!(command & (AHCI_CMD_CR | AHCI_CMD_FR))) return 1;
    }
    return 0;
}

static int port_stop(void)
{
    uint32_t command = port_read32(AHCI_PORT_CMD);
    command &= ~(AHCI_CMD_ST | AHCI_CMD_FRE);
    port_write32(AHCI_PORT_CMD, command);
    return wait_port_idle();
}

static void port_start(void)
{
    uint32_t command = port_read32(AHCI_PORT_CMD);
    command |= AHCI_CMD_FRE;
    port_write32(AHCI_PORT_CMD, command);
    command |= AHCI_CMD_ST;
    port_write32(AHCI_PORT_CMD, command);
}

static int ahci_transfer(uint32_t lba, void* buffer, int write)
{
    uint8_t* fis;
    uint32_t status;
    uintptr_t address = (uintptr_t)buffer;

    if (!initialized || !buffer || (address >> 32) != 0) return 0;
    if (!port_stop()) return 0;
    command_list[0].opts = (5U << 0) | (write ? (1U << 6) : 0);
    command_list[0].prdbc = 0;
    command_list[0].ctba = (uint32_t)(uintptr_t)&command_table;
    command_list[0].ctbau = 0;
    for (uint32_t i = 1; i < 32; i++) command_list[i].opts = 0;
    for (uint32_t i = 0; i < sizeof(command_table); i++)
        ((volatile uint8_t*)&command_table)[i] = 0;

    command_table.dba = (uint32_t)address;
    command_table.dbau = 0;
    command_table.dbc = 511;
    fis = command_table.cfis;
    fis[0] = 0x27;
    fis[1] = 1 << 7;
    fis[2] = write ? 0x35 : 0x25;
    fis[3] = 0x80;
    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 0x40;
    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = fis[10] = fis[11] = 0;
    fis[12] = 1;

    port_write32(AHCI_PORT_SERR, 0xFFFFFFFFU);
    port_start();
    for (uint32_t i = 0; i < 1000000; i++) {
        status = port_read32(AHCI_PORT_TFD);
        if (!(status & (AHCI_TFD_BSY | AHCI_TFD_DRQ))) break;
    }
    if (status & (AHCI_TFD_BSY | AHCI_TFD_DRQ)) return 0;
    port_write32(AHCI_PORT_CI, 1);
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(port_read32(AHCI_PORT_CI) & 1)) break;
        if (port_read32(AHCI_PORT_TFD) & 1) return 0;
    }
    return !(port_read32(AHCI_PORT_CI) & 1) &&
           !(port_read32(AHCI_PORT_TFD) & 1);
}

int ahci_init(void)
{
    pci_device_t device;
    const pci_bar_t* bar;
    uint32_t ports;

    initialized = 0;
    if (!pci_find_class(AHCI_CLASS, AHCI_SUBCLASS, &device) ||
        device.prog_if != AHCI_PROGIF || !pci_enable_device(&device, 1))
        return 0;
    bar = pci_bar_info(&device, 5);
    if (!bar || !bar->present || bar->type != 0 || !bar->base ||
        (bar->base >> 32) != 0) return 0;
    abar = (volatile uint8_t*)(uintptr_t)bar->base;
    mmio_write32(AHCI_GHC, mmio_read32(AHCI_GHC) | AHCI_GHC_AE);
    mmio_write32(AHCI_GHC, mmio_read32(AHCI_GHC) | AHCI_GHC_RESET);
    for (uint32_t i = 0; i < 100000; i++)
        if (!(mmio_read32(AHCI_GHC) & AHCI_GHC_RESET)) break;
    if (mmio_read32(AHCI_GHC) & AHCI_GHC_RESET) return 0;
    mmio_write32(AHCI_GHC, mmio_read32(AHCI_GHC) | AHCI_GHC_AE);
    ports = mmio_read32(AHCI_CAP) ? mmio_read32(0x0C) : 0;
    for (uint32_t i = 0; i < AHCI_PORT_COUNT; i++) {
        volatile uint8_t* candidate;
        if (!(ports & (1U << i))) continue;
        candidate = abar + AHCI_PORT_BASE + i * AHCI_PORT_SIZE;
        port = candidate;
        if ((*(volatile uint32_t*)(candidate + AHCI_PORT_SSTS) & 0x0F) == 3 &&
            *(volatile uint32_t*)(candidate + AHCI_PORT_SIG) == AHCI_SIG_SATA) {
            if (!port_stop()) continue;
            *(volatile uint32_t*)(candidate + 0x00) = (uint32_t)(uintptr_t)command_list;
            *(volatile uint32_t*)(candidate + 0x04) = 0;
            *(volatile uint32_t*)(candidate + 0x08) = 0;
            *(volatile uint32_t*)(candidate + 0x0C) = 0;
            *(volatile uint32_t*)(candidate + 0x14) = 0;
            port_write32(AHCI_PORT_SERR, 0xFFFFFFFFU);
            initialized = 1;
            port_start();
            return 1;
        }
    }
    port = 0;
    return 0;
}

int ahci_available(void) { return initialized; }
int ahci_read28(uint32_t lba, uint8_t* buffer) { return ahci_transfer(lba, buffer, 0); }
int ahci_write28(uint32_t lba, const uint8_t* buffer) {
    return ahci_transfer(lba, (void*)buffer, 1);
}

int ahci_flush(void)
{
    uint32_t status;
    uint8_t* fis;
    if (!initialized || !port || !port_stop()) return 0;
    command_list[0].opts = 5U;
    command_list[0].prdbc = 0;
    command_list[0].ctba = (uint32_t)(uintptr_t)&command_table;
    command_list[0].ctbau = 0;
    for (uint32_t i = 1; i < 32; i++) command_list[i].opts = 0;
    for (uint32_t i = 0; i < sizeof(command_table); i++)
        ((volatile uint8_t*)&command_table)[i] = 0;
    fis = command_table.cfis;
    fis[0] = 0x27;
    fis[1] = 1U << 7;
    fis[2] = 0xEA;
    port_write32(AHCI_PORT_SERR, 0xFFFFFFFFU);
    port_start();
    for (uint32_t i = 0; i < 1000000; i++) {
        status = port_read32(AHCI_PORT_TFD);
        if (!(status & (AHCI_TFD_BSY | AHCI_TFD_DRQ))) break;
    }
    if (status & (AHCI_TFD_BSY | AHCI_TFD_DRQ)) return 0;
    port_write32(AHCI_PORT_CI, 1);
    for (uint32_t i = 0; i < 1000000; i++) {
        if (!(port_read32(AHCI_PORT_CI) & 1U)) break;
        if (port_read32(AHCI_PORT_TFD) & 1U) return 0;
    }
    return !(port_read32(AHCI_PORT_CI) & 1U) &&
           !(port_read32(AHCI_PORT_TFD) & 1U);
}