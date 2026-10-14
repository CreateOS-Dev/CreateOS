// ============================================================
// CreateOS 0.0.1
// ============================================================


#include <stdint.h>
#include "arch/x86_64/io.h"
#include "drivers/ata.h"
#include "drivers/ahci.h"

#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTRL    0x3F6

#define ATA_REG_DATA        (ATA_PRIMARY_IO + 0)
#define ATA_REG_ERROR       (ATA_PRIMARY_IO + 1)
#define ATA_REG_SECCOUNT    (ATA_PRIMARY_IO + 2)
#define ATA_REG_LBA0        (ATA_PRIMARY_IO + 3)
#define ATA_REG_LBA1        (ATA_PRIMARY_IO + 4)
#define ATA_REG_LBA2        (ATA_PRIMARY_IO + 5)
#define ATA_REG_DRIVE       (ATA_PRIMARY_IO + 6)
#define ATA_REG_COMMAND     (ATA_PRIMARY_IO + 7)
#define ATA_REG_STATUS      (ATA_PRIMARY_IO + 7)

#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_READ_SECT   0x20
#define ATA_CMD_WRITE_SECT  0x30
#define ATA_CMD_FLUSH_CACHE 0xE7

#define ATAPI_CMD_PACKET    0xA0
#define ATAPI_CMD_IDENTIFY_PACKET 0xA1

#define ATA_STATUS_ERR      0x01
#define ATA_STATUS_DRQ      0x08
#define ATA_STATUS_SRV      0x10
#define ATA_STATUS_DF       0x20
#define ATA_STATUS_RDY      0x40
#define ATA_STATUS_BSY      0x80

static int g_ata_drive = 0;
static int g_ata_block_ready;

static void ata_io_wait(void) {
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
    inb(ATA_PRIMARY_CTRL);
}

static int ata_wait_not_busy(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(ATA_REG_STATUS);
        if (!(s & ATA_STATUS_BSY))
            return 1;
    }
    return 0;
}

static int ata_wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(ATA_REG_STATUS);
        if (s & ATA_STATUS_ERR) return 0;
        if (s & ATA_STATUS_DRQ) return 1;
    }
    return 0;
}

static void ata_identify(uint16_t *buf) {
    if (!ata_wait_drq())
        return;
    for (int i = 0; i < 256; i++)
        buf[i] = inw(ATA_REG_DATA);
}

static int atapi_wait_not_busy(void) {
    return ata_wait_not_busy();
}
static int atapi_wait_drq(void) {
    return ata_wait_drq();
}
static int atapi_wait_for_service(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t s = inb(ATA_REG_STATUS);
        if (!(s & ATA_STATUS_BSY) && (s & ATA_STATUS_DRQ))
            return 1;
        if (s & ATA_STATUS_ERR)
            return 0;
    }
    return 0;
}

static int drive_is_atapi = 0;

int ata_detect(void) {
    g_ata_block_ready = 0;
    drive_is_atapi = 0;
    for (int drive = 0; drive < 2; drive++) {
        outb(ATA_REG_DRIVE, 0xE0 | (drive << 4));
        ata_io_wait();

        outb(ATA_REG_SECCOUNT, 0);
        outb(ATA_REG_LBA0, 0);
        outb(ATA_REG_LBA1, 0);
        outb(ATA_REG_LBA2, 0);
        outb(ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

        ata_io_wait();

        uint8_t status = inb(ATA_REG_STATUS);
        if (status == 0)
            continue;

        if (!ata_wait_not_busy())
            continue;

        status = inb(ATA_REG_STATUS);
        if (status & ATA_STATUS_ERR)
            continue;

        uint16_t identify[256];
        if (!ata_wait_drq())
            continue;
        ata_identify(identify);

        uint8_t device_type = (identify[0] >> 8) & 0xFF;
        if (device_type == 0x00) {
            drive_is_atapi = 0;
            g_ata_drive = drive;
            g_ata_block_ready = 1;
            return 1;
        } else if (device_type == 0x14) {
        } else {
            continue;
        }
    }
    if (drive_is_atapi) {
        return 1;
    }
    return 0;
}

int ata_block_available(void)
{
    return ahci_available() || g_ata_block_ready;
}

int ata_flush(void)
{
    if (ahci_available()) return ahci_flush();
    if (!g_ata_block_ready || drive_is_atapi || !ata_wait_not_busy()) return 0;
    outb(ATA_REG_COMMAND, ATA_CMD_FLUSH_CACHE);
    if (!ata_wait_not_busy()) return 0;
    return !(inb(ATA_REG_STATUS) & (ATA_STATUS_ERR | ATA_STATUS_DF));
}

int ata_read28(uint32_t lba, uint8_t* buffer) {
    if (!buffer || lba > 0x0FFFFFFF)
        return 0;
    if (ahci_available()) return ahci_read28(lba, buffer);
    if (drive_is_atapi) {
        return 0;
    }
    if (!ata_wait_not_busy())
        return 0;

    outb(ATA_REG_DRIVE, 0xE0 | (g_ata_drive << 4) | ((lba >> 24) & 0x0F));
    outb(ATA_REG_SECCOUNT, 1);
    outb(ATA_REG_LBA0, (uint8_t)lba);
    outb(ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(ATA_REG_LBA2, (uint8_t)(lba >> 16));
    outb(ATA_REG_COMMAND, ATA_CMD_READ_SECT);

    ata_io_wait();

    if (!ata_wait_drq())
        return 0;

    for (int i = 0; i < 256; i++) {
        ((uint16_t*)buffer)[i] = inw(ATA_REG_DATA);
    }

    return 1;
}

int ata_write28(uint32_t lba, const uint8_t* buffer) {
    if (!buffer)
        return 0;
    if (drive_is_atapi)
        return 0;
    if (lba > 0x0FFFFFFF)
        return 0;
    if (ahci_available()) return ahci_write28(lba, buffer);

    if (!ata_wait_not_busy())
        return 0;

    outb(ATA_REG_DRIVE, 0xE0 | (g_ata_drive << 4) | ((lba >> 24) & 0x0F));
    outb(ATA_REG_SECCOUNT, 1);
    outb(ATA_REG_LBA0, (uint8_t)lba);
    outb(ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outb(ATA_REG_LBA2, (uint8_t)(lba >> 16));
    outb(ATA_REG_COMMAND, ATA_CMD_WRITE_SECT);

    ata_io_wait();

    if (!ata_wait_drq())
        return 0;

    for (int i = 0; i < 256; i++) {
        uint16_t w = buffer[i * 2] | (buffer[i * 2 + 1] << 8);
        outw(ATA_REG_DATA, w);
    }

    if (!ata_wait_not_busy())
        return 0;

    return 1;
}

int atapi_read_sector(uint32_t lba, uint8_t* buffer) {
    if (!buffer)
        return 0;
    if (!drive_is_atapi)
        return 0;
    if (!ata_wait_not_busy())
        return 0;

    outb(ATA_REG_DRIVE, 0xE0 | (g_ata_drive << 4));
    ata_io_wait();

    outb(ATA_REG_COMMAND, ATAPI_CMD_PACKET);
    ata_io_wait();

    if (!ata_wait_drq())
        return 0;

    uint8_t packet[12] = {0};
    packet[0] = 0x28;
    packet[1] = 0x00;
    packet[2] = (lba >> 24) & 0xFF;
    packet[3] = (lba >> 16) & 0xFF;
    packet[4] = (lba >> 8) & 0xFF;
    packet[5] = lba & 0xFF;
    packet[6] = 0x00;
    packet[7] = 0x00;
    packet[8] = 0x00;
    packet[9] = 0x01;
    packet[10] = 0x00;
    packet[11] = 0x00;

    for (int i = 0; i < 12; i++)
        outw(ATA_REG_DATA, packet[i]);

    for (int i = 0; i < 12; i++) {
        outw(ATA_REG_DATA, packet[i] & 0xFF);
    }

    if (!atapi_wait_for_service())
        return 0;

    for (int i = 0; i < 2048/2; i++) {
        uint16_t w = inw(ATA_REG_DATA);
        ((uint8_t*)buffer)[i*2] = w & 0xFF;
        ((uint8_t*)buffer)[i*2+1] = (w >> 8) & 0xFF;
    }

    if (!ata_wait_not_busy())
        return 0;

    return 1;
}

int ata_is_atapi(void) {
    return drive_is_atapi;
}