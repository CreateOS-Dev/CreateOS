#include "drivers/virtio_net.h"

#include "arch/x86/io.h"
#include "drivers/netdev.h"
#include "drivers/pci.h"
#include "drivers/serial.h"
#include "lib/string.h"
#include "memory/pmm.h"
#include <stdint.h>

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_NET_LEGACY_DEVICE_ID 0x1000
#define VIRTIO_NET_DEVICE_ID 0x1041
#define VIRTIO_NET_QUEUE_MAX 256
#define VIRTIO_QUEUE_MEMORY_SIZE (3 * FRAME_SIZE)
/* VirtIO 1.0 uses the extended v1 header; legacy transport uses 10 bytes. */
#define VIRTIO_NET_LEGACY_HEADER_SIZE 10
#define VIRTIO_NET_MODERN_HEADER_SIZE 12
#define VIRTIO_NET_BUFFER_SIZE \
    (NETDEV_MAX_FRAME_SIZE + VIRTIO_NET_MODERN_HEADER_SIZE)
#define VIRTIO_POLL_LIMIT 1000000U

#define VIRTIO_HOST_FEATURES 0
#define VIRTIO_GUEST_FEATURES 4
#define VIRTIO_QUEUE_PFN 8
#define VIRTIO_QUEUE_SIZE 12
#define VIRTIO_QUEUE_SELECT 14
#define VIRTIO_QUEUE_NOTIFY 16
#define VIRTIO_DEVICE_STATUS 18
#define VIRTIO_DEVICE_ISR 19
#define VIRTIO_DEVICE_CONFIG 20

#define VIRTIO_STATUS_ACK 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED 128
#define VIRTIO_NET_F_MAC (1U << 5)
#define VIRTIO_NET_F_STATUS (1U << 16)
#define VIRTIO_F_VERSION_1 (1U << 0)
#define VIRTQ_DESC_F_WRITE 2
#define VIRTIO_PCI_CAP_VENDOR 0x09
#define VIRTIO_PCI_CAP_COMMON 1
#define VIRTIO_PCI_CAP_NOTIFY 2
#define VIRTIO_PCI_CAP_ISR 3
#define VIRTIO_PCI_CAP_DEVICE 4
#define VIRTIO_COMMON_CONFIG_MIN_SIZE 56
#define VIRTIO_DEVICE_CONFIG_MIN_SIZE 8

typedef struct __attribute__((packed)) {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
} virtq_desc_t;

typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t length;
} virtq_used_element_t;

typedef struct {
    uint16_t size;
    virtq_desc_t* descriptors;
    volatile uint16_t* available_flags;
    volatile uint16_t* available_index;
    volatile uint16_t* available_ring;
    volatile uint16_t* used_flags;
    volatile uint16_t* used_index;
    volatile virtq_used_element_t* used_ring;
} virtq_t;

typedef struct {
    uint16_t io_base;
    uint16_t rx_size;
    uint16_t tx_size;
    uint16_t rx_last_used;
    uint16_t tx_available;
    uint8_t header_size;
    uint16_t notify_offsets[2];
    virtq_t rx_queue;
    virtq_t tx_queue;
    uint32_t negotiated_features;
    uint32_t notify_multiplier;
    volatile uint8_t* common_config;
    volatile uint8_t* notify_config;
    volatile uint8_t* device_config;
    uint64_t notify_config_size;
    void* mapped_bars[6];
    uint8_t mac[6];
    int modern;
    int ready;
} virtio_net_state_t;

static virtio_net_state_t virtio_net;
static uint8_t rx_queue_memory[VIRTIO_QUEUE_MEMORY_SIZE]
    __attribute__((aligned(FRAME_SIZE)));
static uint8_t tx_queue_memory[VIRTIO_QUEUE_MEMORY_SIZE]
    __attribute__((aligned(FRAME_SIZE)));
static uint8_t rx_buffers[VIRTIO_NET_QUEUE_MAX][VIRTIO_NET_BUFFER_SIZE];
static uint8_t tx_buffer[VIRTIO_NET_BUFFER_SIZE];

static int virtio_map_capability_bar(const pci_device_t* device, uint8_t bar,
                                     uint32_t offset, uint32_t length,
                                     volatile uint8_t** address)
{
    const pci_bar_t* info;
    uint8_t* mapping;
    if (!device || bar >= 6 || !length || !address)
        return 0;
    info = pci_bar_info(device, bar);
    if (!info || !info->present ||
        (info->type != 0 && info->type != 2) ||
        offset > info->size || length > info->size - offset)
        return 0;
    if (!virtio_net.mapped_bars[bar]) {
        virtio_net.mapped_bars[bar] = pci_map_bar(device, bar);
        if (!virtio_net.mapped_bars[bar])
            return 0;
    }
    mapping = virtio_net.mapped_bars[bar];
    *address = mapping + offset;
    return 1;
}

static int virtio_modern_capabilities_init(const pci_device_t* device)
{
    volatile uint8_t* common = 0;
    volatile uint8_t* notify = 0;
    volatile uint8_t* isr = 0;
    volatile uint8_t* config = 0;
    uint32_t common_size = 0;
    uint32_t notify_size = 0;
    uint32_t config_size = 0;
    uint32_t notify_multiplier = 0;
    uint32_t status;
    uint8_t pointer;
    uint8_t seen[64] = {0};
    unsigned int count = 0;

    status = pci_read_config32(device, 0x04);
    if (status == 0xFFFFFFFFU || !((status >> 16) & (1U << 4)))
        return 0;
    pointer = (uint8_t)(pci_read_config32(device, 0x34) & 0xFF);
    while (pointer) {
        uint32_t header;
        uint8_t next;
        uint8_t length;
        uint8_t type;
        uint32_t bar_info;
        uint32_t offset;
        uint32_t region_length;
        uint8_t bar;

        if (++count > 48 || pointer < 0x40 || pointer > 0xFC ||
            (pointer & 3))
            return 0;
        if (seen[(pointer - 0x40) / 4])
            return 0;
        seen[(pointer - 0x40) / 4] = 1;
        header = pci_read_config32(device, pointer);
        next = (uint8_t)((header >> 8) & 0xFF);
        if ((header & 0xFF) != VIRTIO_PCI_CAP_VENDOR) {
            pointer = next;
            continue;
        }
        length = (uint8_t)((header >> 16) & 0xFF);
        type = (uint8_t)(header >> 24);
        if (length < 16 || (uint32_t)pointer + length > 256)
            return 0;
        bar_info = pci_read_config32(device, (uint8_t)(pointer + 4));
        bar = (uint8_t)bar_info;
        offset = pci_read_config32(device, (uint8_t)(pointer + 8));
        region_length = pci_read_config32(device,
                                          (uint8_t)(pointer + 12));

        if (type == VIRTIO_PCI_CAP_COMMON) {
            if (common || region_length < VIRTIO_COMMON_CONFIG_MIN_SIZE ||
                !virtio_map_capability_bar(device, bar, offset,
                                           region_length, &common))
                return 0;
            common_size = region_length;
        } else if (type == VIRTIO_PCI_CAP_NOTIFY) {
            if (notify || length < 20 || region_length < 2 ||
                !virtio_map_capability_bar(device, bar, offset,
                                           region_length, &notify))
                return 0;
            notify_multiplier = pci_read_config32(
                device, (uint8_t)(pointer + 16));
            if (notify_multiplier == 0xFFFFFFFFU || !notify_multiplier)
                return 0;
            notify_size = region_length;
        } else if (type == VIRTIO_PCI_CAP_ISR) {
            if (isr || region_length < 1 ||
                !virtio_map_capability_bar(device, bar, offset,
                                           region_length, &isr))
                return 0;
        } else if (type == VIRTIO_PCI_CAP_DEVICE) {
            if (config || region_length < VIRTIO_DEVICE_CONFIG_MIN_SIZE ||
                !virtio_map_capability_bar(device, bar, offset,
                                           region_length, &config))
                return 0;
            config_size = region_length;
        }
        pointer = next;
    }
    if (!common || !notify || !isr || !config || !common_size ||
        !notify_size || !config_size)
        return 0;
    virtio_net.common_config = common;
    virtio_net.notify_config = notify;
    virtio_net.device_config = config;
    virtio_net.notify_config_size = notify_size;
    virtio_net.notify_multiplier = notify_multiplier;
    virtio_net.modern = 1;
    return 1;
}

static uint8_t virtio_read8(uint16_t offset)
{
    if (virtio_net.modern) {
        if (offset == VIRTIO_DEVICE_STATUS)
            return virtio_net.common_config[20];
        if (offset >= VIRTIO_DEVICE_CONFIG)
            return virtio_net.device_config[offset - VIRTIO_DEVICE_CONFIG];
        return 0xFF;
    }
    return inb((uint16_t)(virtio_net.io_base + offset));
}

static uint16_t virtio_read16(uint16_t offset)
{
    if (virtio_net.modern) {
        if (offset == VIRTIO_QUEUE_SIZE)
            return *(volatile uint16_t*)(virtio_net.common_config + 24);
        if (offset == VIRTIO_DEVICE_CONFIG + 6)
            return *(volatile uint16_t*)(virtio_net.device_config + 6);
        return 0xFFFF;
    }
    return inw((uint16_t)(virtio_net.io_base + offset));
}

static uint32_t virtio_read32(uint16_t offset)
{
    if (virtio_net.modern) {
        if (offset == VIRTIO_HOST_FEATURES) {
            *(volatile uint32_t*)(virtio_net.common_config) = 0;
            return *(volatile uint32_t*)(virtio_net.common_config + 4);
        }
        return 0xFFFFFFFFU;
    }
    return inl((uint16_t)(virtio_net.io_base + offset));
}

static void virtio_write8(uint16_t offset, uint8_t value)
{
    if (virtio_net.modern && offset == VIRTIO_DEVICE_STATUS) {
        virtio_net.common_config[20] = value;
        return;
    }
    outb((uint16_t)(virtio_net.io_base + offset), value);
}

static void virtio_write16(uint16_t offset, uint16_t value)
{
    if (virtio_net.modern) {
        if (offset == VIRTIO_QUEUE_SELECT) {
            *(volatile uint16_t*)(virtio_net.common_config + 22) = value;
            return;
        }
        return;
    }
    outw((uint16_t)(virtio_net.io_base + offset), value);
}

static void virtio_write32(uint16_t offset, uint32_t value)
{
    if (virtio_net.modern) {
        if (offset == VIRTIO_GUEST_FEATURES) {
            *(volatile uint32_t*)(virtio_net.common_config + 8) = 0;
            *(volatile uint32_t*)(virtio_net.common_config + 12) = value;
        }
        return;
    }
    outl((uint16_t)(virtio_net.io_base + offset), value);
}

static int virtio_queue_init(virtq_t* queue, uint16_t queue_index,
                             uint8_t* memory)
{
    uint64_t physical = (uint64_t)(uintptr_t)memory;
    uint32_t descriptors_size;
    uint32_t available_offset;
    uint32_t used_offset;
    uint32_t required_size;

    if (!queue || !memory || (physical & (FRAME_SIZE - 1)) ||
        (!virtio_net.modern && physical > UINT32_MAX))
        return 0;

    virtio_write16(VIRTIO_QUEUE_SELECT, queue_index);
    queue->size = virtio_read16(VIRTIO_QUEUE_SIZE);
    if (!queue->size || queue->size > VIRTIO_NET_QUEUE_MAX ||
        (queue->size & (queue->size - 1)))
        return 0;

    descriptors_size = (uint32_t)queue->size * sizeof(virtq_desc_t);
    available_offset = descriptors_size;
    used_offset = (available_offset + 4U + 2U * queue->size +
                   FRAME_SIZE - 1U) & ~(FRAME_SIZE - 1U);
    required_size = used_offset + 4U +
                    (uint32_t)queue->size * sizeof(virtq_used_element_t);
    if (required_size > VIRTIO_QUEUE_MEMORY_SIZE)
        return 0;

    queue->descriptors = (virtq_desc_t*)memory;
    queue->available_flags = (volatile uint16_t*)(memory + available_offset);
    queue->available_index = queue->available_flags + 1;
    queue->available_ring = queue->available_flags + 2;
    queue->used_flags = (volatile uint16_t*)(memory + used_offset);
    queue->used_index = queue->used_flags + 1;
    queue->used_ring =
        (volatile virtq_used_element_t*)(queue->used_flags + 2);

    if (virtio_net.modern) {
        volatile uint8_t* common = virtio_net.common_config;
        uint16_t notify_offset;
        if (*(volatile uint16_t*)(common + 28) != 0)
            return 0;
        notify_offset = *(volatile uint16_t*)(common + 30);
        if (notify_offset &&
            virtio_net.notify_multiplier >
                (UINT64_MAX - 2U) / notify_offset)
            return 0;
        if ((uint64_t)notify_offset * virtio_net.notify_multiplier + 2U >
            virtio_net.notify_config_size)
            return 0;
        virtio_net.notify_offsets[queue_index] = notify_offset;
        *(volatile uint32_t*)(common + 32) = (uint32_t)physical;
        *(volatile uint32_t*)(common + 36) = (uint32_t)(physical >> 32);
        *(volatile uint32_t*)(common + 40) =
            (uint32_t)(physical + available_offset);
        *(volatile uint32_t*)(common + 44) =
            (uint32_t)((physical + available_offset) >> 32);
        *(volatile uint32_t*)(common + 48) =
            (uint32_t)(physical + used_offset);
        *(volatile uint32_t*)(common + 52) =
            (uint32_t)((physical + used_offset) >> 32);
        io_memory_barrier();
        *(volatile uint16_t*)(common + 28) = 1;
        io_memory_barrier();
        return *(volatile uint16_t*)(common + 28) == 1;
    }
    virtio_write32(VIRTIO_QUEUE_PFN, (uint32_t)(physical >> 12));
    return virtio_read32(VIRTIO_QUEUE_PFN) == (uint32_t)(physical >> 12);
}

static void virtio_queue_notify(uint16_t queue_index)
{
    if (!virtio_net.modern) {
        virtio_write16(VIRTIO_QUEUE_NOTIFY, queue_index);
        return;
    }
    {
        uint64_t byte_offset =
            (uint64_t)virtio_net.notify_offsets[queue_index] *
            virtio_net.notify_multiplier;
        volatile uint16_t* notify =
            (volatile uint16_t*)(virtio_net.notify_config + byte_offset);
        io_memory_barrier();
        *notify = queue_index;
    }
}

static int virtio_net_send(void* context, const void* frame, size_t length)
{
    uint16_t previous_used;
    uint16_t slot;
    virtio_net_state_t* state = context;
    if (!state || !state->ready || !frame ||
        length < NETDEV_ETHERNET_HEADER_SIZE ||
        length > NETDEV_MAX_FRAME_SIZE)
        return 0;

    memset(tx_buffer, 0, state->header_size);
    memcpy(tx_buffer + state->header_size, frame, length);
    state->tx_queue.descriptors[0].address = (uint64_t)(uintptr_t)tx_buffer;
    state->tx_queue.descriptors[0].length =
        (uint32_t)(length + state->header_size);
    state->tx_queue.descriptors[0].flags = 0;
    state->tx_queue.descriptors[0].next = 0;

    previous_used = *state->tx_queue.used_index;
    slot = (uint16_t)(state->tx_available % state->tx_size);
    state->tx_queue.available_ring[slot] = 0;
    io_memory_barrier();
    *state->tx_queue.available_index = ++state->tx_available;
    io_memory_barrier();
    virtio_queue_notify(1);

    for (uint32_t i = 0; i < VIRTIO_POLL_LIMIT; i++) {
        if (*state->tx_queue.used_index != previous_used) {
            io_memory_barrier();
            if (state->tx_queue.used_ring[previous_used % state->tx_size].id
                == 0)
                return (int)length;
            state->ready = 0;
            virtio_write8(VIRTIO_DEVICE_STATUS,
                          VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                          VIRTIO_STATUS_FAILED);
            serial_puts("[virtio-net] invalid TX completion\n");
            return 0;
        }
        cpu_pause();
    }
    state->ready = 0;
    virtio_write8(VIRTIO_DEVICE_STATUS,
                  VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                  VIRTIO_STATUS_FAILED);
    serial_puts("[virtio-net] TX queue timed out\n");
    return 0;
}

static int virtio_net_receive(void* context, void* frame, size_t capacity)
{
    virtio_net_state_t* state = context;
    virtq_used_element_t element;
    uint32_t frame_length;
    uint16_t descriptor;
    uint16_t slot;
    if (!state || !state->ready || !frame ||
        capacity < NETDEV_ETHERNET_HEADER_SIZE)
        return 0;
    if (*state->rx_queue.used_index == state->rx_last_used)
        return 0;

    io_memory_barrier();
    element = state->rx_queue.used_ring[state->rx_last_used % state->rx_size];
    state->rx_last_used++;
    if (element.id >= state->rx_size) {
        state->ready = 0;
        virtio_write8(VIRTIO_DEVICE_STATUS,
                      VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                      VIRTIO_STATUS_FAILED);
        serial_puts("[virtio-net] invalid RX completion\n");
        return -1;
    }
    descriptor = (uint16_t)element.id;
    if (element.length < state->header_size)
        frame_length = 0;
    else
        frame_length = element.length - state->header_size;

    if (frame_length < NETDEV_ETHERNET_HEADER_SIZE ||
        frame_length > NETDEV_MAX_FRAME_SIZE || frame_length > capacity) {
        frame_length = 0;
    } else {
        memcpy(frame, rx_buffers[descriptor] + state->header_size,
               frame_length);
    }

    slot = (uint16_t)(*state->rx_queue.available_index % state->rx_size);
    state->rx_queue.available_ring[slot] = descriptor;
    io_memory_barrier();
    state->rx_queue.available_index++;
    io_memory_barrier();
    virtio_queue_notify(0);
    return (int)frame_length;
}

static int virtio_net_link_up(void* context)
{
    virtio_net_state_t* state = context;
    if (!state || !state->ready)
        return 0;
    if (state->negotiated_features & VIRTIO_NET_F_STATUS)
        return (virtio_read16(VIRTIO_DEVICE_CONFIG + 6) & 1U) != 0;
    return 1;
}

static const netdev_ops_t virtio_net_ops = {
    .send = virtio_net_send,
    .receive = virtio_net_receive,
    .link_up = virtio_net_link_up
};

static int virtio_net_mac_valid(const uint8_t mac[6])
{
    uint8_t combined = 0;
    for (size_t i = 0; i < 6; i++)
        combined |= mac[i];
    return combined != 0 && !(mac[0] & 1U);
}

static int virtio_net_start(int modern)
{
    static const char device_name[] = "virtio-net";
    uint32_t host_features;
    uint32_t guest_features = 0;
    uint32_t host_features_high = 0;
    uint8_t status;

    virtio_net.header_size = modern
        ? VIRTIO_NET_MODERN_HEADER_SIZE : VIRTIO_NET_LEGACY_HEADER_SIZE;
    virtio_write8(VIRTIO_DEVICE_STATUS, 0);
    virtio_write8(VIRTIO_DEVICE_STATUS, VIRTIO_STATUS_ACK);
    virtio_write8(VIRTIO_DEVICE_STATUS,
                  VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    if (modern) {
        *(volatile uint32_t*)virtio_net.common_config = 0;
        host_features = *(volatile uint32_t*)(virtio_net.common_config + 4);
        *(volatile uint32_t*)virtio_net.common_config = 1;
        host_features_high =
            *(volatile uint32_t*)(virtio_net.common_config + 4);
        if (!(host_features_high & VIRTIO_F_VERSION_1)) {
            serial_puts("[virtio-net] modern device lacks VERSION_1\n");
            goto fail;
        }
    } else {
        host_features = virtio_read32(VIRTIO_HOST_FEATURES);
    }

    if (host_features & VIRTIO_NET_F_MAC)
        guest_features |= VIRTIO_NET_F_MAC;
    if (host_features & VIRTIO_NET_F_STATUS)
        guest_features |= VIRTIO_NET_F_STATUS;
    if (!(guest_features & VIRTIO_NET_F_MAC)) {
        serial_puts("[virtio-net] device does not provide a MAC address\n");
        goto fail;
    }
    virtio_net.negotiated_features = guest_features;
    virtio_write32(VIRTIO_GUEST_FEATURES, guest_features);
    if (modern) {
        *(volatile uint32_t*)(virtio_net.common_config + 8) = 1;
        *(volatile uint32_t*)(virtio_net.common_config + 12) =
            VIRTIO_F_VERSION_1;
        io_memory_barrier();
        status = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                 VIRTIO_STATUS_FEATURES_OK;
        virtio_write8(VIRTIO_DEVICE_STATUS, status);
        if (!(virtio_read8(VIRTIO_DEVICE_STATUS) &
              VIRTIO_STATUS_FEATURES_OK)) {
            serial_puts("[virtio-net] device rejected negotiated features\n");
            goto fail;
        }
    }
    {
        int config_stable = !modern;
        for (int attempt = 0; attempt < 8 && !config_stable; attempt++) {
            uint8_t generation = virtio_net.common_config[21];
            for (size_t i = 0; i < sizeof(virtio_net.mac); i++)
                virtio_net.mac[i] = virtio_read8(
                    VIRTIO_DEVICE_CONFIG + (uint16_t)i);
            io_memory_barrier();
            config_stable = generation == virtio_net.common_config[21];
        }
        if (!modern) {
            for (size_t i = 0; i < sizeof(virtio_net.mac); i++)
                virtio_net.mac[i] = virtio_read8(
                    VIRTIO_DEVICE_CONFIG + (uint16_t)i);
        }
        if (!config_stable) {
            serial_puts("[virtio-net] device configuration kept changing\n");
            goto fail;
        }
    }
    if (!virtio_net_mac_valid(virtio_net.mac) ||
        netdev_mac_in_use(virtio_net.mac)) {
        serial_puts("[virtio-net] invalid or duplicate MAC address\n");
        goto fail;
    }

    memset(rx_queue_memory, 0, sizeof(rx_queue_memory));
    memset(tx_queue_memory, 0, sizeof(tx_queue_memory));
    if (!virtio_queue_init(&virtio_net.rx_queue, 0, rx_queue_memory) ||
        !virtio_queue_init(&virtio_net.tx_queue, 1, tx_queue_memory)) {
        serial_puts("[virtio-net] unsupported or unavailable virtqueue\n");
        goto fail;
    }
    virtio_net.rx_size = virtio_net.rx_queue.size;
    virtio_net.tx_size = virtio_net.tx_queue.size;
    virtio_net.rx_last_used = 0;
    virtio_net.tx_available = 0;

    for (uint16_t i = 0; i < virtio_net.rx_size; i++) {
        virtio_net.rx_queue.descriptors[i].address =
            (uint64_t)(uintptr_t)rx_buffers[i];
        virtio_net.rx_queue.descriptors[i].length = sizeof(rx_buffers[i]);
        virtio_net.rx_queue.descriptors[i].flags = VIRTQ_DESC_F_WRITE;
        virtio_net.rx_queue.descriptors[i].next = 0;
        virtio_net.rx_queue.available_ring[i] = i;
    }
    *virtio_net.rx_queue.available_index = virtio_net.rx_size;
    io_memory_barrier();
    virtio_net.ready = 1;
    status = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER;
    if (modern)
        status |= VIRTIO_STATUS_FEATURES_OK;
    status |= VIRTIO_STATUS_DRIVER_OK;
    virtio_write8(VIRTIO_DEVICE_STATUS, status);
    if (!(virtio_read8(VIRTIO_DEVICE_STATUS) & VIRTIO_STATUS_DRIVER_OK)) {
        serial_puts("[virtio-net] device did not enter DRIVER_OK\n");
        goto fail;
    }
    virtio_queue_notify(0);

    if (netdev_register(device_name, virtio_net.mac,
                        NETDEV_MAX_FRAME_SIZE, &virtio_net_ops,
                        &virtio_net) < 0) {
        serial_puts("[virtio-net] network-device registration failed\n");
        goto fail;
    }
    serial_puts(modern
        ? "[virtio-net] modern PCI transport initialized\n"
        : "[virtio-net] legacy PCI transport initialized\n");
    return 1;

fail:
    virtio_write8(VIRTIO_DEVICE_STATUS,
                  VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                  VIRTIO_STATUS_FAILED);
    virtio_net.ready = 0;
    return 0;
}

int virtio_net_init(void)
{
    if (virtio_net.ready)
        return 1;

    for (int i = 0; i < pci_device_count(); i++) {
        const pci_device_t* device = pci_device_at(i);
        const pci_bar_t* bar;
        if (!device || device->vendor != VIRTIO_VENDOR_ID)
            continue;
        if (device->device != VIRTIO_NET_LEGACY_DEVICE_ID &&
            device->device != VIRTIO_NET_DEVICE_ID)
            continue;

        memset(&virtio_net, 0, sizeof(virtio_net));
        if (!pci_enable_device(device, 1)) {
            serial_puts("[virtio-net] PCI I/O/memory/bus-master enable failed\n");
            continue;
        }
        if (virtio_modern_capabilities_init(device) &&
            virtio_net_start(1))
            return 1;
        if (virtio_net.modern)
            virtio_write8(VIRTIO_DEVICE_STATUS, 0);
        if (device->device == VIRTIO_NET_DEVICE_ID)
            continue;

        memset(&virtio_net, 0, sizeof(virtio_net));
        bar = pci_bar_info(device, 0);
        if (!bar || !bar->present || bar->type != 1 || !bar->base ||
            bar->base > UINT16_MAX || bar->size < VIRTIO_DEVICE_CONFIG + 8 ||
            bar->size > 0x10000 - bar->base) {
            serial_puts("[virtio-net] unsupported legacy I/O BAR\n");
            continue;
        }
        virtio_net.io_base = (uint16_t)bar->base;
        if (virtio_net_start(0))
            return 1;
    }
    return 0;
}
