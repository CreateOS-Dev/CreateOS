#include "drivers/e1000.h"
#include "drivers/pci.h"
#include "drivers/serial.h"
#include "arch/x86_64/random.h"
#include "lib/string.h"
#include "memory/pmm.h"
#include "arch/x86/io.h"
#include <stddef.h>
#include <stdint.h>

#define E1000_VENDOR_INTEL 0x8086
#define E1000_DEVICE_82540EM 0x100E
#define E1000_RING_COUNT 16
#define E1000_DMA_BUFFER_SIZE 2048
#define E1000_POLL_LIMIT 1000000U

#define E1000_REG_CTRL 0x0000
#define E1000_REG_STATUS 0x0008
#define E1000_STATUS_LINK_UP (1U << 1)
#define E1000_REG_RCTL 0x0100
#define E1000_REG_TCTL 0x0400
#define E1000_REG_TIPG 0x0410
#define E1000_REG_RAL0 0x5400
#define E1000_REG_RAH0 0x5404
#define E1000_RAH_AV (1U << 31)
#define E1000_REG_RDBAL 0x2800
#define E1000_REG_RDBAH 0x2804
#define E1000_REG_RDLEN 0x2808
#define E1000_REG_RDH 0x2810
#define E1000_REG_RDT 0x2818
#define E1000_REG_TDBAL 0x3800
#define E1000_REG_TDBAH 0x3804
#define E1000_REG_TDLEN 0x3808
#define E1000_REG_TDH 0x3810
#define E1000_REG_TDT 0x3818

#define E1000_CTRL_RST (1U << 26)
#define E1000_RCTL_EN (1U << 1)
#define E1000_RCTL_BAM (1U << 15)
#define E1000_RCTL_SECRC (1U << 26)
#define E1000_TCTL_EN (1U << 1)
#define E1000_TCTL_PSP (1U << 3)
#define E1000_TX_CMD_EOP 0x01
#define E1000_TX_CMD_IFCS 0x02
#define E1000_TX_CMD_RS 0x08
#define E1000_DESC_DONE 0x01
#define E1000_RX_STATUS_EOP 0x02

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint16_t length;
    uint8_t checksum_offset;
    uint8_t command;
    uint8_t status;
    uint8_t checksum_start;
    uint16_t special;
} e1000_tx_desc_t;

typedef struct {
    volatile uint8_t* registers;
    uint64_t bar_size;
    e1000_rx_desc_t* rx_ring;
    e1000_tx_desc_t* tx_ring;
    uint8_t* rx_buffers[E1000_RING_COUNT];
    uint8_t* tx_buffer;
    uint8_t mac[6];
    int mac_is_local;
    uint16_t rx_index;
    uint16_t tx_index;
    int ready;
} e1000_state_t;

static e1000_state_t e1000;

static uint32_t e1000_read(uint32_t offset);

static int e1000_netdev_send(void* context, const void* frame, size_t length)
{
    (void)context;
    return e1000_send(frame, length);
}

static int e1000_netdev_receive(void* context, void* frame, size_t capacity)
{
    (void)context;
    return e1000_receive(frame, capacity);
}

static int e1000_netdev_link_up(void* context)
{
    (void)context;
    return e1000.ready &&
           (e1000_read(E1000_REG_STATUS) & E1000_STATUS_LINK_UP) != 0;
}

static const netdev_ops_t e1000_netdev_ops = {
    .send = e1000_netdev_send,
    .receive = e1000_netdev_receive,
    .link_up = e1000_netdev_link_up
};

static uint32_t e1000_read(uint32_t offset)
{
    return mmio_read32(e1000.registers + offset);
}

static void e1000_write(uint32_t offset, uint32_t value)
{
    mmio_write32(e1000.registers + offset, value);
}

static void e1000_release_dma(void)
{
    for (uint32_t i = 0; i < E1000_RING_COUNT; i++) {
        if (e1000.rx_buffers[i]) pmm_free_frame(e1000.rx_buffers[i]);
        e1000.rx_buffers[i] = NULL;
    }
    if (e1000.tx_buffer) pmm_free_frame(e1000.tx_buffer);
    if (e1000.rx_ring) pmm_free_frame(e1000.rx_ring);
    if (e1000.tx_ring) pmm_free_frame(e1000.tx_ring);
    e1000.tx_buffer = NULL;
    e1000.rx_ring = NULL;
    e1000.tx_ring = NULL;
}

static void* e1000_alloc_dma_page(void)
{
    void* page = pmm_alloc_frame();
    if (!page) return NULL;
    for (size_t i = 0; i < FRAME_SIZE; i++)
        ((uint8_t*)page)[i] = 0;
    return page;
}

static int e1000_reset(void)
{
    e1000_write(E1000_REG_CTRL, e1000_read(E1000_REG_CTRL) | E1000_CTRL_RST);
    for (uint32_t i = 0; i < E1000_POLL_LIMIT; i++) {
        if (!(e1000_read(E1000_REG_CTRL) & E1000_CTRL_RST)) return 1;
        cpu_pause();
    }
    return 0;
}

static int e1000_read_mac(void)
{
    uint32_t low = e1000_read(E1000_REG_RAL0);
    uint32_t high = e1000_read(E1000_REG_RAH0);
    uint8_t nonzero = 0;

    e1000.mac[0] = (uint8_t)low;
    e1000.mac[1] = (uint8_t)(low >> 8);
    e1000.mac[2] = (uint8_t)(low >> 16);
    e1000.mac[3] = (uint8_t)(low >> 24);
    e1000.mac[4] = (uint8_t)high;
    e1000.mac[5] = (uint8_t)(high >> 8);
    for (size_t i = 0; i < sizeof(e1000.mac); i++) nonzero |= e1000.mac[i];
    return nonzero && !(e1000.mac[0] & 1U) &&
           (high & E1000_RAH_AV) != 0;
}

static int e1000_generate_mac(void)
{
    uint32_t low;
    uint32_t high;
    uint8_t generated[6];

    for (int attempt = 0; attempt < 8; attempt++) {
        if (!x86_64_random_bytes(generated, sizeof(generated)))
            return 0;
        generated[0] = (uint8_t)((generated[0] | 0x02U) & 0xFEU);
        if (netdev_mac_in_use(generated)) continue;
        memcpy(e1000.mac, generated, sizeof(e1000.mac));
        low = (uint32_t)e1000.mac[0] |
              ((uint32_t)e1000.mac[1] << 8) |
              ((uint32_t)e1000.mac[2] << 16) |
              ((uint32_t)e1000.mac[3] << 24);
        high = (uint32_t)e1000.mac[4] |
               ((uint32_t)e1000.mac[5] << 8) | E1000_RAH_AV;
        e1000_write(E1000_REG_RAL0, low);
        e1000_write(E1000_REG_RAH0, high);
        e1000.mac_is_local = 1;
        return 1;
    }
    return 0;
}

int e1000_init(void)
{
    const pci_device_t* device = NULL;
    const pci_bar_t* bar;
    int found = 0;

    if (e1000.ready) return 1;
    for (int i = 0; i < pci_device_count(); i++) {
        const pci_device_t* candidate = pci_device_at(i);
        if (candidate && candidate->vendor == E1000_VENDOR_INTEL &&
            candidate->device == E1000_DEVICE_82540EM) {
            device = candidate;
            found = 1;
            break;
        }
    }
    if (!found) return 0;

    bar = pci_bar_info(device, 0);
    if (!bar || !bar->present || bar->type != 0 || !bar->base ||
        bar->base > 0xFFFFFFFFULL || bar->size < 0x5408 ||
        bar->base + bar->size > 0x100000000ULL) {
        serial_puts("[e1000] unsupported or missing MMIO BAR\n");
        return 0;
    }
    if (!pci_enable_device(device, 1)) {
        serial_puts("[e1000] PCI memory/bus-master enable failed\n");
        return 0;
    }

    e1000.registers = (volatile uint8_t*)pci_map_bar(device, 0);
    e1000.bar_size = bar->size;
    if (!e1000.registers) {
        serial_puts("[e1000] MMIO BAR mapping failed\n");
        return 0;
    }
    if (!e1000_reset()) {
        serial_puts("[e1000] controller reset timed out\n");
        e1000.registers = NULL;
        return 0;
    }
    (void)e1000_read(E1000_REG_STATUS);
    e1000.mac_is_local = 0;
    if ((!e1000_read_mac() || netdev_mac_in_use(e1000.mac)) &&
        !e1000_generate_mac()) {
        serial_puts("[e1000] ERROR: hardware MAC unavailable or already in use; RDRAND fallback failed\n");
        e1000.registers = NULL;
        return 0;
    }

    e1000.rx_ring = (e1000_rx_desc_t*)e1000_alloc_dma_page();
    e1000.tx_ring = (e1000_tx_desc_t*)e1000_alloc_dma_page();
    e1000.tx_buffer = (uint8_t*)e1000_alloc_dma_page();
    if (!e1000.rx_ring || !e1000.tx_ring || !e1000.tx_buffer) {
        serial_puts("[e1000] DMA ring allocation failed\n");
        goto fail;
    }

    for (uint32_t i = 0; i < E1000_RING_COUNT; i++) {
        e1000.rx_buffers[i] = (uint8_t*)e1000_alloc_dma_page();
        if (!e1000.rx_buffers[i]) {
            serial_puts("[e1000] DMA receive buffer allocation failed\n");
            goto fail;
        }
        e1000.rx_ring[i].address =
            (uint64_t)(uintptr_t)e1000.rx_buffers[i];
    }
    if ((uint64_t)(uintptr_t)e1000.rx_ring > 0xFFFFFFFFULL ||
        (uint64_t)(uintptr_t)e1000.tx_ring > 0xFFFFFFFFULL ||
        (uint64_t)(uintptr_t)e1000.tx_buffer > 0xFFFFFFFFULL) {
        serial_puts("[e1000] DMA descriptors are above 4 GiB\n");
        goto fail;
    }
    for (uint32_t i = 0; i < E1000_RING_COUNT; i++) {
        if ((uint64_t)(uintptr_t)e1000.rx_buffers[i] > 0xFFFFFFFFULL) {
            serial_puts("[e1000] DMA receive buffer is above 4 GiB\n");
            goto fail;
        }
    }
    for (uint32_t i = 0; i < E1000_RING_COUNT; i++)
        e1000.tx_ring[i].status = E1000_DESC_DONE;

    e1000.rx_index = 0;
    e1000.tx_index = 0;
    e1000_write(E1000_REG_RCTL, 0);
    e1000_write(E1000_REG_TCTL, 0);
    io_memory_barrier();
    e1000_write(E1000_REG_RDBAL,
                (uint32_t)(uintptr_t)e1000.rx_ring);
    e1000_write(E1000_REG_RDBAH,
                (uint32_t)((uint64_t)(uintptr_t)e1000.rx_ring >> 32));
    e1000_write(E1000_REG_RDLEN,
                E1000_RING_COUNT * sizeof(e1000_rx_desc_t));
    e1000_write(E1000_REG_RDH, 0);
    e1000_write(E1000_REG_RDT, E1000_RING_COUNT - 1);
    e1000_write(E1000_REG_TDBAL,
                (uint32_t)(uintptr_t)e1000.tx_ring);
    e1000_write(E1000_REG_TDBAH,
                (uint32_t)((uint64_t)(uintptr_t)e1000.tx_ring >> 32));
    e1000_write(E1000_REG_TDLEN,
                E1000_RING_COUNT * sizeof(e1000_tx_desc_t));
    e1000_write(E1000_REG_TDH, 0);
    e1000_write(E1000_REG_TDT, 0);
    e1000_write(E1000_REG_TIPG, 10 | (8U << 10) | (6U << 20));
    io_memory_barrier();
    e1000_write(E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                (0x10U << 4) | (0x40U << 12));
    e1000_write(E1000_REG_RCTL,
                E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
    io_memory_barrier();
    e1000.ready = 1;
    int netdev_index = netdev_register("e1000", e1000.mac,
                                       E1000_FRAME_MAX,
                                       &e1000_netdev_ops, &e1000);
    if (netdev_index < 0) {
        serial_puts("[e1000] ERROR: unable to register network device\n");
    } else {
        (void)netdev_set_mac_is_local(netdev_index, e1000.mac_is_local);
        if (e1000.mac_is_local)
            serial_puts("[e1000] using generated locally administered MAC\n");
    }
    return 1;

fail:
    e1000_release_dma();
    return 0;
}

int e1000_available(void)
{
    return e1000.ready;
}

const uint8_t* e1000_mac_address(void)
{
    return e1000.ready ? e1000.mac : NULL;
}

int e1000_send(const void* frame, size_t length)
{
    e1000_tx_desc_t* desc;
    if (!e1000.ready || !frame || length < 14 ||
        length > E1000_FRAME_MAX)
        return 0;

    desc = &e1000.tx_ring[e1000.tx_index];
    for (uint32_t i = 0; i < E1000_POLL_LIMIT; i++) {
        if (desc->status & E1000_DESC_DONE) break;
        if (i + 1 == E1000_POLL_LIMIT) return 0;
        cpu_pause();
    }
    for (size_t i = 0; i < length; i++)
        e1000.tx_buffer[i] = ((const uint8_t*)frame)[i];
    desc->address = (uint64_t)(uintptr_t)e1000.tx_buffer;
    desc->length = (uint16_t)length;
    desc->checksum_offset = 0;
    desc->command = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    desc->status = 0;
    desc->checksum_start = 0;
    desc->special = 0;
    io_memory_barrier();
    e1000.tx_index = (uint16_t)((e1000.tx_index + 1) % E1000_RING_COUNT);
    e1000_write(E1000_REG_TDT, e1000.tx_index);

    for (uint32_t i = 0; i < E1000_POLL_LIMIT; i++) {
        if (desc->status & E1000_DESC_DONE) return (int)length;
        cpu_pause();
    }
    return 0;
}

int e1000_receive(void* frame, size_t capacity)
{
    e1000_rx_desc_t* desc;
    uint16_t length;
    uint8_t errors;

    if (!e1000.ready || !frame || !capacity) return -1;
    desc = &e1000.rx_ring[e1000.rx_index];
    io_memory_barrier();
    if (!(desc->status & E1000_DESC_DONE)) return 0;

    length = desc->length;
    errors = desc->errors;
    if (errors || !(desc->status & E1000_RX_STATUS_EOP) || length < 14 ||
        length > E1000_DMA_BUFFER_SIZE ||
        length > E1000_FRAME_MAX || length > capacity) {
        desc->length = 0;
        desc->checksum = 0;
        desc->status = 0;
        desc->errors = 0;
        io_memory_barrier();
        e1000_write(E1000_REG_RDT, e1000.rx_index);
        e1000.rx_index = (uint16_t)((e1000.rx_index + 1) % E1000_RING_COUNT);
        return -1;
    }
    for (uint16_t i = 0; i < length; i++)
        ((uint8_t*)frame)[i] = e1000.rx_buffers[e1000.rx_index][i];
    desc->length = 0;
    desc->checksum = 0;
    desc->status = 0;
    desc->errors = 0;
    io_memory_barrier();
    e1000_write(E1000_REG_RDT, e1000.rx_index);
    e1000.rx_index = (uint16_t)((e1000.rx_index + 1) % E1000_RING_COUNT);
    return length;
}
