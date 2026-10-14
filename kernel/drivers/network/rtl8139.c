#include "drivers/rtl8139.h"
#include "drivers/netdev.h"
#include "drivers/pci.h"
#include "drivers/serial.h"
#include "arch/x86/cpu.h"
#include "arch/x86/io.h"
#include "arch/x86_64/random.h"
#include "lib/string.h"
#include <stddef.h>
#include <stdint.h>

#define RTL8139_VENDOR 0x10EC
#define RTL8139_DEVICE_8139 0x8139
#define RTL8139_RING_SIZE 8192U
#define RTL8139_TX_SLOTS 4U
#define RTL8139_TX_BUFFER_SIZE 2048U
#define RTL8139_POLL_LIMIT 1000000U
#define RTL8139_MIN_FRAME_SIZE 60U
#define RTL8139_MAX_WIRE_SIZE (NETDEV_MAX_FRAME_SIZE + 4U)

#define RTL8139_REG_MAC0 0x00
#define RTL8139_REG_MAR0 0x08
#define RTL8139_REG_TX_STATUS0 0x10
#define RTL8139_REG_TX_ADDRESS0 0x20
#define RTL8139_REG_RX_BUFFER 0x30
#define RTL8139_REG_COMMAND 0x37
#define RTL8139_REG_CAPR 0x38
#define RTL8139_REG_IMR 0x3C
#define RTL8139_REG_ISR 0x3E
#define RTL8139_REG_TX_CONFIG 0x40
#define RTL8139_REG_RX_CONFIG 0x44
#define RTL8139_REG_MEDIA_STATUS 0x58
#define RTL8139_REG_CONFIG1 0x52

#define RTL8139_COMMAND_BUFFER_EMPTY 0x01
#define RTL8139_COMMAND_TX_ENABLE 0x04
#define RTL8139_COMMAND_RX_ENABLE 0x08
#define RTL8139_COMMAND_RESET 0x10
#define RTL8139_RX_ACCEPT_PHYSICAL 0x02
#define RTL8139_RX_ACCEPT_MULTICAST 0x04
#define RTL8139_RX_ACCEPT_BROADCAST 0x08
#define RTL8139_RX_WRAP 0x80
#define RTL8139_TX_OWNED 0x00002000U
#define RTL8139_TX_UNDERRUN 0x00004000U
#define RTL8139_TX_OK 0x00008000U
#define RTL8139_TX_ABORTED 0x40000000U
#define RTL8139_MEDIA_LINK_DOWN 0x04

typedef struct {
    uint16_t io_base;
    uint8_t mac[6];
    uint8_t rx_ring[RTL8139_RING_SIZE] __attribute__((aligned(256)));
    uint8_t tx_buffers[RTL8139_TX_SLOTS][RTL8139_TX_BUFFER_SIZE]
        __attribute__((aligned(16)));
    uint16_t rx_offset;
    uint8_t tx_slot;
    int ready;
} rtl8139_state_t;

static rtl8139_state_t rtl8139;

static uint8_t rtl8139_read8(uint16_t offset)
{
    return inb((uint16_t)(rtl8139.io_base + offset));
}

static uint32_t rtl8139_read32(uint16_t offset)
{
    return inl((uint16_t)(rtl8139.io_base + offset));
}

static void rtl8139_write8(uint16_t offset, uint8_t value)
{
    outb((uint16_t)(rtl8139.io_base + offset), value);
}

static void rtl8139_write16(uint16_t offset, uint16_t value)
{
    outw((uint16_t)(rtl8139.io_base + offset), value);
}

static void rtl8139_write32(uint16_t offset, uint32_t value)
{
    outl((uint16_t)(rtl8139.io_base + offset), value);
}

static int rtl8139_reset(void)
{
    rtl8139_write8(RTL8139_REG_COMMAND, RTL8139_COMMAND_RESET);
    for (uint32_t i = 0; i < RTL8139_POLL_LIMIT; i++) {
        if (!(rtl8139_read8(RTL8139_REG_COMMAND) & RTL8139_COMMAND_RESET))
            return 1;
        cpu_pause();
    }
    return 0;
}

static int rtl8139_generate_mac(void)
{
    uint8_t generated[6];
    uint32_t low;
    uint32_t high;

    if (!x86_64_random_bytes(generated, sizeof(generated))) return 0;
    generated[0] = (uint8_t)((generated[0] | 0x02U) & 0xFEU);
    if (netdev_mac_in_use(generated)) return 0;
    memcpy(rtl8139.mac, generated, sizeof(rtl8139.mac));
    low = (uint32_t)rtl8139.mac[0] |
          ((uint32_t)rtl8139.mac[1] << 8) |
          ((uint32_t)rtl8139.mac[2] << 16) |
          ((uint32_t)rtl8139.mac[3] << 24);
    high = (uint32_t)rtl8139.mac[4] |
           ((uint32_t)rtl8139.mac[5] << 8);
    rtl8139_write32(RTL8139_REG_MAC0, low);
    rtl8139_write32((uint16_t)(RTL8139_REG_MAC0 + 4), high);
    return 1;
}

static int rtl8139_netdev_send(void* context, const void* frame,
                               size_t length)
{
    uint8_t slot;
    size_t wire_length;
    uint32_t status;

    (void)context;
    if (!rtl8139.ready || !frame || length < NETDEV_ETHERNET_HEADER_SIZE ||
        length > NETDEV_MAX_FRAME_SIZE)
        return 0;

    slot = rtl8139.tx_slot;
    for (uint32_t i = 0; i < RTL8139_POLL_LIMIT; i++) {
        status = rtl8139_read32((uint16_t)(RTL8139_REG_TX_STATUS0 +
                                          slot * 4U));
        if (!(status & RTL8139_TX_OWNED)) break;
        if (i + 1 == RTL8139_POLL_LIMIT) return 0;
        cpu_pause();
    }

    wire_length = length < RTL8139_MIN_FRAME_SIZE
        ? RTL8139_MIN_FRAME_SIZE : length;
    memcpy(rtl8139.tx_buffers[slot], frame, length);
    if (wire_length > length)
        memset(rtl8139.tx_buffers[slot] + length, 0, wire_length - length);
    io_memory_barrier();
    rtl8139_write32((uint16_t)(RTL8139_REG_TX_STATUS0 + slot * 4U),
                    (uint32_t)wire_length);

    for (uint32_t i = 0; i < RTL8139_POLL_LIMIT; i++) {
        status = rtl8139_read32((uint16_t)(RTL8139_REG_TX_STATUS0 +
                                          slot * 4U));
        if (status & (RTL8139_TX_OK | RTL8139_TX_ABORTED |
                      RTL8139_TX_UNDERRUN))
            break;
        if (i + 1 == RTL8139_POLL_LIMIT) return 0;
        cpu_pause();
    }
    rtl8139.tx_slot = (uint8_t)((slot + 1U) % RTL8139_TX_SLOTS);
    return (status & RTL8139_TX_OK) ? (int)length : 0;
}

static int rtl8139_ring_copy(void* destination, uint16_t offset,
                             size_t length)
{
    uint8_t* output = (uint8_t*)destination;
    if (!destination || offset >= RTL8139_RING_SIZE ||
        length > RTL8139_RING_SIZE)
        return 0;
    for (size_t i = 0; i < length; i++)
        output[i] = rtl8139.rx_ring[(offset + i) % RTL8139_RING_SIZE];
    return 1;
}

static int rtl8139_netdev_receive(void* context, void* frame, size_t capacity)
{
    uint16_t header[2];
    uint16_t wire_length;
    uint16_t next_offset;
    size_t frame_length;

    (void)context;
    if (!rtl8139.ready || !frame || capacity < NETDEV_ETHERNET_HEADER_SIZE)
        return -1;
    io_memory_barrier();
    if (rtl8139_read8(RTL8139_REG_COMMAND) &
        RTL8139_COMMAND_BUFFER_EMPTY)
        return 0;

    if (!rtl8139_ring_copy(header, rtl8139.rx_offset, sizeof(header)))
        return -1;
    wire_length = header[1];
    if (!(header[0] & 1U) || wire_length < 18 ||
        wire_length > RTL8139_MAX_WIRE_SIZE)
        goto discard;

    frame_length = wire_length - 4U;
    if (frame_length > NETDEV_MAX_FRAME_SIZE || frame_length > capacity)
        goto discard;
    if (!rtl8139_ring_copy(frame,
                           (uint16_t)((rtl8139.rx_offset + 4U) %
                                      RTL8139_RING_SIZE),
                           frame_length))
        goto discard;

    next_offset = (uint16_t)((rtl8139.rx_offset + wire_length + 4U + 3U) &
                             ~3U);
    rtl8139.rx_offset = (uint16_t)(next_offset % RTL8139_RING_SIZE);
    rtl8139_write16(RTL8139_REG_CAPR,
                    (uint16_t)(rtl8139.rx_offset - 16U));
    return (int)frame_length;

discard:
    rtl8139.rx_offset = (uint16_t)((rtl8139.rx_offset + 4U) &
                                   ~(uint16_t)3U);
    rtl8139.rx_offset = (uint16_t)(rtl8139.rx_offset % RTL8139_RING_SIZE);
    rtl8139_write16(RTL8139_REG_CAPR,
                    (uint16_t)(rtl8139.rx_offset - 16U));
    return -1;
}

static int rtl8139_netdev_link_up(void* context)
{
    (void)context;
    return rtl8139.ready &&
           !(rtl8139_read8(RTL8139_REG_MEDIA_STATUS) &
             RTL8139_MEDIA_LINK_DOWN);
}

static const netdev_ops_t rtl8139_netdev_ops = {
    .send = rtl8139_netdev_send,
    .receive = rtl8139_netdev_receive,
    .link_up = rtl8139_netdev_link_up
};

int rtl8139_init(void)
{
    const pci_device_t* device = NULL;
    const pci_bar_t* io_bar = NULL;
    uint8_t mac_nonzero = 0;

    if (rtl8139.ready) return 1;
    for (int i = 0; i < pci_device_count(); i++) {
        const pci_device_t* candidate = pci_device_at(i);
        if (candidate && candidate->vendor == RTL8139_VENDOR &&
            candidate->device == RTL8139_DEVICE_8139) {
            device = candidate;
            break;
        }
    }
    if (!device) return 0;

    for (int i = 0; i < 6; i++) {
        const pci_bar_t* candidate = pci_bar_info(device, i);
        if (candidate && candidate->present && (candidate->type == 0 ||
            candidate->type == 1)) {
            io_bar = candidate;
            break;
        }
    }
    if (!io_bar || !io_bar->present || !pci_enable_device(device, 1))
        return 0;

    if (io_bar->base < 0x100 || io_bar->base > 0xFF9) {
        if (io_bar->type == 0 && io_bar->base == 0 && io_bar->size < 0x60)
            return 0;
    }

    memset(&rtl8139, 0, sizeof(rtl8139));
    rtl8139.io_base = (uint16_t)io_bar->base;
    if (!rtl8139_reset()) {
        rtl8139.io_base = 0;
        return 0;
    }
    rtl8139_write8(RTL8139_REG_CONFIG1, 0);
    for (size_t i = 0; i < sizeof(rtl8139.mac); i++) {
        rtl8139.mac[i] = rtl8139_read8((uint16_t)(RTL8139_REG_MAC0 + i));
        mac_nonzero |= rtl8139.mac[i];
    }
    if (!mac_nonzero || (rtl8139.mac[0] & 1U) ||
        netdev_mac_in_use(rtl8139.mac)) {
        if (!rtl8139_generate_mac()) {
            serial_puts("[rtl8139] ERROR: invalid or duplicate hardware MAC\n");
            rtl8139.io_base = 0;
            return 0;
        }
    }
    if ((uint64_t)(uintptr_t)rtl8139.rx_ring > 0xFFFFFFFFULL ||
        (uint64_t)(uintptr_t)rtl8139.tx_buffers > 0xFFFFFFFFULL) {
        serial_puts("[rtl8139] ERROR: DMA buffers are above 4 GiB\n");
        rtl8139.io_base = 0;
        return 0;
    }

    rtl8139_write8(RTL8139_REG_COMMAND, 0);
    rtl8139_write32(RTL8139_REG_RX_BUFFER,
                    (uint32_t)(uintptr_t)rtl8139.rx_ring);
    rtl8139_write32(RTL8139_REG_MAR0, 0xFFFFFFFFU);
    rtl8139_write32((uint16_t)(RTL8139_REG_MAR0 + 4), 0xFFFFFFFFU);
    rtl8139_write32(RTL8139_REG_RX_CONFIG,
                    RTL8139_RX_ACCEPT_PHYSICAL |
                    RTL8139_RX_ACCEPT_MULTICAST |
                    RTL8139_RX_ACCEPT_BROADCAST | RTL8139_RX_WRAP);
    rtl8139_write32(RTL8139_REG_TX_CONFIG, 0x03000700U);
    rtl8139_write16(RTL8139_REG_CAPR, 0xFFF0U);
    rtl8139_write16(RTL8139_REG_ISR, 0xFFFFU);
    rtl8139_write16(RTL8139_REG_IMR, 0);
    for (uint32_t i = 0; i < RTL8139_TX_SLOTS; i++) {
        if ((uint64_t)(uintptr_t)rtl8139.tx_buffers[i] > 0xFFFFFFFFULL) {
            rtl8139.io_base = 0;
            return 0;
        }
        rtl8139_write32((uint16_t)(RTL8139_REG_TX_ADDRESS0 + i * 4U),
                        (uint32_t)(uintptr_t)rtl8139.tx_buffers[i]);
    }
    io_memory_barrier();
    rtl8139_write8(RTL8139_REG_COMMAND,
                   RTL8139_COMMAND_TX_ENABLE |
                   RTL8139_COMMAND_RX_ENABLE);
    rtl8139.ready = 1;

    if (netdev_register("rtl8139", rtl8139.mac, NETDEV_MAX_FRAME_SIZE,
                        &rtl8139_netdev_ops, &rtl8139) < 0) {
        rtl8139_write8(RTL8139_REG_COMMAND, 0);
        memset(&rtl8139, 0, sizeof(rtl8139));
        serial_puts("[rtl8139] ERROR: unable to register network device\n");
        return 0;
    }
    netdev_set_mac_is_local(netdev_count() - 1, 1);
    serial_puts("[rtl8139] RTL8139 Ethernet controller initialized\n");
    return 1;
}

int rtl8139_available(void)
{
    return rtl8139.ready;
}
