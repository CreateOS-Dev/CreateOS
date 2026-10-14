// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "drivers/usb.h"
#include "drivers/usb_hid.h"
#include "drivers/block.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/pci.h"
#include "drivers/serial.h"
#include "drivers/timer.h"
#include "fs/vfs.h"
#include "arch/x86_64/io.h"
#include "memory/pmm.h"
#include "lib/string.h"
#include <stddef.h>

#define PCI_CLASS_SERIAL_BUS 0x0C
#define PCI_SUBCLASS_USB      0x03
#define USB_PROGIF_UHCI       0x00
#define USB_PROGIF_OHCI       0x10
#define USB_PROGIF_EHCI       0x20
#define USB_PROGIF_XHCI       0x30
#define UHCI_USBCMD           0x00
#define UHCI_USBSTS           0x02
#define UHCI_USBINTR          0x04
#define UHCI_SOFMOD           0x0C
#define UHCI_PORTSC1          0x10
#define UHCI_PORTSC2          0x12
#define UHCI_PORTSC_CCS       (1U << 0)
#define UHCI_PORTSC_CSC       (1U << 1)
#define UHCI_PORTSC_PE        (1U << 2)
#define UHCI_PORTSC_PEC       (1U << 3)
#define UHCI_PORTSC_PR        (1U << 9)
#define UHCI_PORTSC_SUSP      (1U << 12)
#define UHCI_PORTSC_CHANGE_BITS (UHCI_PORTSC_CSC | UHCI_PORTSC_PEC)
#define UHCI_PORTSC_WRITABLE_STATE (UHCI_PORTSC_PE | UHCI_PORTSC_SUSP)
#define XHCI_TRBS_PER_RING    256
#define XHCI_MAX_SLOTS        16
#define XHCI_TRB_TYPE_SHIFT   10
#define XHCI_TRB_TYPE_MASK    0x3F
#define XHCI_TRB_ENABLE_SLOT  9
#define XHCI_TRB_DISABLE_SLOT 10
#define XHCI_TRB_COMMAND_COMPLETION 33
#define XHCI_TRB_TRANSFER_EVENT 32
#define XHCI_COMPLETION_SUCCESS 1
#define XHCI_COMPLETION_SHORT_PACKET 13
#define XHCI_COMMAND_TIMEOUT  1000000
#define XHCI_TRANSFER_TIMEOUT 10000000
#define XHCI_TRB_LINK 6
#define XHCI_TRB_SETUP_STAGE 2
#define XHCI_TRB_DATA_STAGE 3
#define XHCI_TRB_STATUS_STAGE 4
#define XHCI_TRB_NORMAL 1
#define XHCI_TRB_ADDRESS_DEVICE 11
#define XHCI_TRB_CONFIGURE_ENDPOINT 12
#define XHCI_TRB_EVALUATE_CONTEXT 13
#define XHCI_ENDPOINT_CERR 3U
#define XHCI_USB_CLASS_MASS_STORAGE 0x08
#define XHCI_USB_SUBCLASS_SCSI 0x06
#define XHCI_USB_PROTOCOL_BOT 0x50
#define XHCI_USB_CLASS_HID 0x03
#define XHCI_USB_HID_SUBCLASS_BOOT 0x01
#define XHCI_USB_HID_PROTOCOL_KEYBOARD 0x01
#define XHCI_USB_HID_PROTOCOL_MOUSE 0x02
#define USB_STORAGE_SECTOR_SIZE 512
#define XHCI_PORTSC_PP         (1U << 9)
#define XHCI_PORTSC_PR         (1U << 4)
#define XHCI_PORTSC_PED        (1U << 1)
#define XHCI_PORTSC_CCS        (1U << 0)
#define XHCI_PORTSC_PRC        (1U << 21)
#define XHCI_PORTSC_WPR        (1U << 31)
#define XHCI_PORTSC_WRC        (1U << 19)
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_SPEED_MASK 0x0FU
#define EHCI_PORTSC_PP         (1U << 12)
#define EHCI_PORTSC_PR         (1U << 8)
#define EHCI_PORTSC_PE         (1U << 2)
#define EHCI_PORTSC_CCS        (1U << 0)
#define EHCI_PORTSC_CSC        (1U << 1)
#define EHCI_PORTSC_PEC        (1U << 3)
#define EHCI_PORTSC_OCC        (1U << 5)
#define EHCI_PORTSC_WRITABLE_STATE (EHCI_PORTSC_PE | (1U << 6) | \
                                    (1U << 7) | EHCI_PORTSC_PR | \
                                    EHCI_PORTSC_PP | (3U << 14) | \
                                    (7U << 20))
#define XHCI_PORTSC_CSC        (1U << 17)
#define XHCI_PORTSC_PLC        (1U << 22)
#define XHCI_PORTSC_CEC        (1U << 23)
#define XHCI_PORTSC_WRITABLE_STATE (XHCI_PORTSC_PP | (3U << 14) | \
                                    (7U << 25))
#define XHCI_PORTSC_CHANGE_BITS (XHCI_PORTSC_CSC | XHCI_PORTSC_PRC | \
                                 XHCI_PORTSC_WRC | XHCI_PORTSC_PLC | \
                                 XHCI_PORTSC_CEC)
#define EHCI_PORTSC_CHANGE_BITS (EHCI_PORTSC_CSC | EHCI_PORTSC_PEC | \
                                 EHCI_PORTSC_OCC)

typedef struct {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

typedef struct {
    pci_device_t pci;
    const char* name;
    volatile uint8_t* mmio;
    uint64_t bar_size;
    uint32_t operational;
    int port_count;
    int initialized;
    uint64_t doorbell_offset;
    uint64_t runtime_offset;
    uint32_t max_slots;
    xhci_trb_t* command_ring;
    xhci_trb_t* event_ring;
    uint64_t* device_context_array;
    uint64_t* event_ring_segment_table;
    uint32_t command_index;
    uint32_t event_index;
    uint32_t command_cycle;
    uint32_t event_cycle;
    uint32_t context_size;
    uint8_t slot_id;
    uint16_t ep0_packet_size;
    uint8_t bulk_in_endpoint;
    uint8_t bulk_out_endpoint;
    uint16_t bulk_in_packet_size;
    uint16_t bulk_out_packet_size;
    uint64_t sector_count;
    uint32_t transfer_index[32];
    uint32_t transfer_cycle[32];
    xhci_trb_t* transfer_rings[32];
    uint8_t* device_context;
    uint8_t* transfer_buffer;
    void* pending_contexts[8];
    uint8_t pending_context_count;
    char block_name[16];
    int storage_ready;
    int storage_registered;
    int media_online;
    int slot_port;
    int partition_count;
    uint32_t bot_tag;
    uint8_t hid_endpoint;
    uint8_t hid_protocol;
    uint8_t hid_interface;
    uint8_t hid_packet_size;
    uint8_t hid_pending;
    uint8_t hid_failed;
    uint8_t hid_caps_lock;
    uint8_t hid_previous[6];
    uint8_t* hid_buffer;
    uint64_t hid_trb_address;
} usb_controller_t;

typedef struct {
    usb_controller_t* controller;
    uint64_t first_lba;
    uint64_t sector_count;
    char name[16];
    int registered;
} usb_partition_t;

static usb_controller_t controllers[8];
static usb_partition_t partitions[8][8];
static int controller_total;
static uint8_t connected_state[8][256];
static uint64_t enumeration_attempt_tick[8][256];

static int usb_storage_transfer(usb_controller_t* controller, uint64_t lba,
                                uint8_t* buffer, int write);
static uint32_t usb_read_le32(const uint8_t* bytes);
static int xhci_hid_submit(usb_controller_t* controller);
static void xhci_hid_handle_event(usb_controller_t* controller,
                                  const xhci_trb_t* event);

static uint32_t usb_crc32(const uint8_t* data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^
                  (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
    }
    return ~crc;
}

static uint16_t usb_read_le16(const uint8_t* bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint64_t usb_read_le64(const uint8_t* bytes)
{
    return (uint64_t)usb_read_le32(bytes) |
           ((uint64_t)usb_read_le32(bytes + 4) << 32);
}

static int uhci_reset(uint16_t io_base)
{
    outw(io_base + UHCI_USBCMD, 0x0002);
    for (uint32_t i = 0; i < 100000; i++) {
        if (!(inw(io_base + UHCI_USBCMD) & 0x0002)) break;
    }
    if (inw(io_base + UHCI_USBCMD) & 0x0002) return 0;
    outw(io_base + UHCI_USBSTS, 0xFFFF);
    outw(io_base + UHCI_USBINTR, 0x0000);
    outb(io_base + UHCI_SOFMOD, 64);
    return 1;
}

static uint32_t usb_mmio_read(volatile uint8_t* base, uint32_t offset)
{
    return *(volatile uint32_t*)(base + offset);
}

static uint16_t uhci_port_offset(int port)
{
    return (uint16_t)(port == 0 ? UHCI_PORTSC1 : UHCI_PORTSC2);
}

static void usb_mmio_write(volatile uint8_t* base, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t*)(base + offset) = value;
}

static void usb_mmio_write64(volatile uint8_t* base, uint32_t offset,
                             uint64_t value)
{
    *(volatile uint64_t*)(base + offset) = value;
}

static int xhci_wait_status(usb_controller_t* controller, uint32_t mask,
                            uint32_t expected)
{
    for (uint32_t i = 0; i < 100000000; i++) {
        if ((usb_mmio_read(controller->mmio,
                           controller->operational + 0x04) & mask) == expected)
            return 1;
    }
    return 0;
}

static void* xhci_alloc_dma_page(void);

static void xhci_track_context(usb_controller_t* controller, void* context)
{
    if (controller->pending_context_count <
        sizeof(controller->pending_contexts) /
            sizeof(controller->pending_contexts[0]))
        controller->pending_contexts[controller->pending_context_count++] =
            context;
}

static void xhci_release_context(usb_controller_t* controller, void* context)
{
    for (uint8_t i = 0; i < controller->pending_context_count; i++) {
        if (controller->pending_contexts[i] != context) continue;
        controller->pending_contexts[i] =
            controller->pending_contexts[--controller->pending_context_count];
        controller->pending_contexts[controller->pending_context_count] = NULL;
        break;
    }
    pmm_free_frame(context);
}

static int xhci_context_is_pending(const usb_controller_t* controller,
                                   const void* context)
{
    for (uint8_t i = 0; i < controller->pending_context_count; i++)
        if (controller->pending_contexts[i] == context) return 1;
    return 0;
}

static void xhci_release_pending_contexts(usb_controller_t* controller)
{
    while (controller->pending_context_count) {
        void* context =
            controller->pending_contexts[--controller->pending_context_count];
        controller->pending_contexts[controller->pending_context_count] = NULL;
        pmm_free_frame(context);
    }
}

static int xhci_halt(usb_controller_t* controller)
{
    uint32_t command_offset = controller->operational;
    uint32_t command = usb_mmio_read(controller->mmio, command_offset);

    usb_mmio_write(controller->mmio, command_offset, command & ~1U);
    return xhci_wait_status(controller, 1U, 1U);
}

static void xhci_publish_event_dequeue(usb_controller_t* controller)
{
    uint32_t erdp = (uint32_t)controller->runtime_offset + 0x20 + 0x18;
    uint64_t address = (uint64_t)(uintptr_t)
        &controller->event_ring[controller->event_index];
    __asm__ volatile("mfence" ::: "memory");
    usb_mmio_write64(controller->mmio, erdp, address | (1U << 3));
}

static int xhci_next_event(usb_controller_t* controller, xhci_trb_t* event)
{
    volatile xhci_trb_t* current =
        &controller->event_ring[controller->event_index];
    uint32_t control = current->control;
    if ((control & 1U) != controller->event_cycle) return 0;
    __asm__ volatile("mfence" ::: "memory");
    event->parameter = current->parameter;
    event->status = current->status;
    event->control = control;
    controller->event_index++;
    if (controller->event_index == XHCI_TRBS_PER_RING) {
        controller->event_index = 0;
        controller->event_cycle ^= 1U;
    }
    xhci_publish_event_dequeue(controller);
    return 1;
}

static int xhci_command_trb(usb_controller_t* controller, uint32_t type,
                            uint8_t slot_id, uint64_t parameter,
                            uint32_t extra_control,
                            uint8_t* completion_slot)
{
    xhci_trb_t command;
    xhci_trb_t* submitted;
    uint32_t doorbell;
    uint32_t control;
    uint64_t submitted_address;

    if (!controller || !controller->initialized || !controller->command_ring)
        return 0;

    if (controller->command_index >= XHCI_TRBS_PER_RING - 1) {
        xhci_trb_t* link = &controller->command_ring[XHCI_TRBS_PER_RING - 1];
        link->control = (6U << XHCI_TRB_TYPE_SHIFT) | (1U << 1) |
                        controller->command_cycle;
        __asm__ volatile("mfence" ::: "memory");
        controller->command_cycle ^= 1U;
        controller->command_index = 0;
    }
    submitted = &controller->command_ring[controller->command_index];
    submitted_address = (uint64_t)(uintptr_t)submitted;
    command.parameter = parameter;
    command.status = 0;
    command.control = (type << XHCI_TRB_TYPE_SHIFT) | extra_control |
                      ((uint32_t)slot_id << 24);
    submitted->parameter = command.parameter;
    submitted->status = command.status;
    __asm__ volatile("mfence" ::: "memory");
    submitted->control = command.control | controller->command_cycle;
    __asm__ volatile("mfence" ::: "memory");
    controller->command_index++;

    doorbell = (uint32_t)controller->doorbell_offset;
    usb_mmio_write(controller->mmio, doorbell, 0);
    for (uint32_t i = 0; i < XHCI_COMMAND_TIMEOUT; i++) {
        if (!xhci_next_event(controller, &command)) {
            __asm__ volatile("pause");
            continue;
        }
        control = command.control;
        if (((control >> XHCI_TRB_TYPE_SHIFT) & XHCI_TRB_TYPE_MASK) !=
            XHCI_TRB_COMMAND_COMPLETION) {
            xhci_hid_handle_event(controller, &command);
            continue;
        }
        if ((command.parameter & ~0xFULL) != submitted_address)
            continue;
        if ((command.status >> 24) != XHCI_COMPLETION_SUCCESS)
            return 0;
        if (completion_slot)
            *completion_slot = (uint8_t)(control >> 24);
        return 1;
    }
    return 0;
}

static int xhci_command(usb_controller_t* controller, uint32_t type,
                        uint8_t slot_id, uint8_t* completion_slot)
{
    return xhci_command_trb(controller, type, slot_id, 0, 0,
                            completion_slot);
}

static void* xhci_alloc_dma_page(void)
{
    void* page = pmm_alloc_frame();
    if (!page) return NULL;
    for (uint32_t i = 0; i < FRAME_SIZE; i++)
        ((uint8_t*)page)[i] = 0;
    return page;
}

static void xhci_release_rings(usb_controller_t* controller)
{
    if (controller->command_ring)
        pmm_free_frame(controller->command_ring);
    if (controller->event_ring)
        pmm_free_frame(controller->event_ring);
    if (controller->device_context_array)
        pmm_free_frame(controller->device_context_array);
    if (controller->event_ring_segment_table)
        pmm_free_frame(controller->event_ring_segment_table);
    controller->command_ring = NULL;
    controller->event_ring = NULL;
    controller->device_context_array = NULL;
    controller->event_ring_segment_table = NULL;
}

static uint32_t* xhci_context(uint8_t* context, uint32_t stride,
                              uint32_t index)
{
    return (uint32_t*)(context + (size_t)stride * index);
}

static int xhci_transfer(usb_controller_t* controller, uint8_t slot_id,
                         uint8_t endpoint_id, const xhci_trb_t* submitted,
                         uint32_t trb_count, uint32_t* transferred)
{
    xhci_trb_t* ring;
    uint32_t index;
    uint32_t cycle;
    uint64_t last_address;
    uint32_t doorbell;
    xhci_trb_t event;

    if (!controller || !controller->initialized || !slot_id ||
        endpoint_id == 0 || endpoint_id >= 32 || !submitted ||
        !trb_count || trb_count >= XHCI_TRBS_PER_RING - 1 ||
        !(ring = controller->transfer_rings[endpoint_id]))
        return 0;

    index = controller->transfer_index[endpoint_id];
    cycle = controller->transfer_cycle[endpoint_id];
    if (index + trb_count >= XHCI_TRBS_PER_RING - 1) {
        xhci_trb_t* link = &ring[XHCI_TRBS_PER_RING - 1];
        link->parameter = (uint64_t)(uintptr_t)ring;
        link->status = 0;
        link->control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) |
                        (1U << 1) | cycle;
        __asm__ volatile("mfence" ::: "memory");
        index = 0;
        cycle ^= 1U;
        controller->transfer_index[endpoint_id] = 0;
        controller->transfer_cycle[endpoint_id] = cycle;
    }

    last_address = (uint64_t)(uintptr_t)&ring[index + trb_count - 1];
    for (uint32_t i = 0; i < trb_count; i++) {
        ring[index + i].parameter = submitted[i].parameter;
        ring[index + i].status = submitted[i].status;
        ring[index + i].control =
            (submitted[i].control & ~1U) | cycle;
    }
    __asm__ volatile("mfence" ::: "memory");
    controller->transfer_index[endpoint_id] = index + trb_count;
    doorbell = (uint32_t)controller->doorbell_offset +
               (uint32_t)slot_id * 4;
    if (doorbell > controller->bar_size - 4) return 0;
    usb_mmio_write(controller->mmio, doorbell, endpoint_id);

    for (uint32_t i = 0; i < XHCI_TRANSFER_TIMEOUT; i++) {
        if (!xhci_next_event(controller, &event)) {
            __asm__ volatile("pause");
            continue;
        }
        if (((event.control >> XHCI_TRB_TYPE_SHIFT) &
             XHCI_TRB_TYPE_MASK) != XHCI_TRB_TRANSFER_EVENT ||
            (uint8_t)(event.control >> 24) != slot_id ||
            ((event.control >> 16) & 0x1FU) != endpoint_id ||
            (event.parameter & ~0xFULL) != last_address) {
            xhci_hid_handle_event(controller, &event);
            continue;
        }
        uint32_t completion = event.status >> 24;
        if (completion != XHCI_COMPLETION_SUCCESS &&
            completion != XHCI_COMPLETION_SHORT_PACKET)
            return 0;
        if (transferred)
            *transferred = event.status & 0x00FFFFFFU;
        return 1;
    }
    return 0;
}

static int xhci_hid_submit(usb_controller_t* controller)
{
    uint8_t endpoint_id;
    xhci_trb_t* ring;
    uint32_t index, cycle, doorbell;

    if (!controller || !controller->hid_endpoint ||
        !controller->hid_buffer || controller->hid_pending ||
        controller->hid_failed || !controller->slot_id)
        return 0;
    endpoint_id = (uint8_t)(((controller->hid_endpoint & 0x0FU) * 2U) + 1U);
    if (endpoint_id >= 32 ||
        !(ring = controller->transfer_rings[endpoint_id]))
        return 0;
    index = controller->transfer_index[endpoint_id];
    cycle = controller->transfer_cycle[endpoint_id];
    if (index >= XHCI_TRBS_PER_RING - 1) {
        xhci_trb_t* link = &ring[XHCI_TRBS_PER_RING - 1];
        link->parameter = (uint64_t)(uintptr_t)ring;
        link->status = 0;
        link->control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) |
                        (1U << 1) | cycle;
        __asm__ volatile("mfence" ::: "memory");
        index = 0;
        cycle ^= 1U;
        controller->transfer_cycle[endpoint_id] = cycle;
    }
    ring[index].parameter = (uint64_t)(uintptr_t)controller->hid_buffer;
    ring[index].status = controller->hid_packet_size;
    ring[index].control = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) |
                          (1U << 5) | cycle;
    controller->hid_trb_address = (uint64_t)(uintptr_t)&ring[index];
    controller->transfer_index[endpoint_id] = index + 1;
    __asm__ volatile("mfence" ::: "memory");
    doorbell = (uint32_t)controller->doorbell_offset +
               (uint32_t)controller->slot_id * 4;
    if (doorbell > controller->bar_size - 4) return 0;
    usb_mmio_write(controller->mmio, doorbell, endpoint_id);
    controller->hid_pending = 1;
    return 1;
}

static int xhci_hid_was_pressed(const uint8_t usages[6], uint8_t usage)
{
    for (int i = 0; i < 6; i++)
        if (usages[i] == usage) return 1;
    return 0;
}

static void xhci_hid_handle_event(usb_controller_t* controller,
                                  const xhci_trb_t* event)
{
    uint32_t type;
    uint8_t event_endpoint;
    uint32_t residual;
    uint32_t actual;

    if (!controller || !event || !controller->hid_pending ||
        !controller->hid_endpoint)
        return;
    type = (event->control >> XHCI_TRB_TYPE_SHIFT) & XHCI_TRB_TYPE_MASK;
    event_endpoint = (uint8_t)((event->control >> 16) & 0x1FU);
    if (type != XHCI_TRB_TRANSFER_EVENT ||
        (uint8_t)(event->control >> 24) != controller->slot_id ||
        event_endpoint !=
            (uint8_t)(((controller->hid_endpoint & 0x0FU) * 2U) + 1U) ||
        (event->parameter & ~0xFULL) != controller->hid_trb_address)
        return;

    controller->hid_pending = 0;
    residual = event->status & 0x00FFFFFFU;
    actual = residual <= controller->hid_packet_size ?
             controller->hid_packet_size - residual : 0;
    if ((event->status >> 24) == XHCI_COMPLETION_SUCCESS ||
        (event->status >> 24) == XHCI_COMPLETION_SHORT_PACKET) {
        if (controller->hid_protocol == XHCI_USB_HID_PROTOCOL_KEYBOARD) {
            usb_hid_keyboard_report_t report;
            if (usb_hid_parse_keyboard_report(controller->hid_buffer, actual,
                                              &report)) {
                uint8_t modifiers = report.modifiers;
                int shift = (modifiers & 0x22U) != 0;
                int control = (modifiers & 0x11U) != 0;
                for (int i = 0; i < 6; i++) {
                    uint8_t usage = report.usages[i];
                    if (usage == 0x39 &&
                        !xhci_hid_was_pressed(controller->hid_previous,
                                              usage))
                        controller->hid_caps_lock ^= 1U;
                    if (!usage ||
                        xhci_hid_was_pressed(controller->hid_previous,
                                             usage))
                        continue;
                    if (control && usage == 0x06) {
                        keyboard_inject_char(KEY_CTRL_C);
                    } else if (control && usage == 0x19) {
                        keyboard_inject_char(KEY_CTRL_V);
                    } else {
                        int key_shift = shift;
                        if (usage >= 0x04 && usage <= 0x1D)
                            key_shift ^= controller->hid_caps_lock;
                        char key = usb_hid_usage_to_ascii(
                            usage, key_shift ? 0x02 : 0);
                        if (control && key >= 'a' && key <= 'z')
                            key = (char)(key & 0x1F);
                        else if (control && key >= 'A' && key <= 'Z')
                            key = (char)((key - 'A' + 1) & 0x1F);
                        if (key) keyboard_inject_char((unsigned char)key);
                    }
                }
                memcpy(controller->hid_previous, report.usages,
                       sizeof(controller->hid_previous));
            }
        } else if (controller->hid_protocol ==
                   XHCI_USB_HID_PROTOCOL_MOUSE) {
            usb_hid_mouse_report_t report;
            if (usb_hid_parse_mouse_report(controller->hid_buffer, actual,
                                           &report)) {
                mouse_event_t mouse = {
                    .dx = report.x,
                    .dy = -(int)report.y,
                    .wheel = report.wheel,
                    .buttons = report.buttons
                };
                mouse_inject_event(&mouse);
            }
        }
    } else {
        controller->hid_failed = 1;
        serial_puts("[usb] ERROR: HID interrupt transfer failed\n");
        return;
    }
    if (!xhci_hid_submit(controller)) {
        controller->hid_failed = 1;
        serial_puts("[usb] ERROR: cannot rearm HID interrupt transfer\n");
    }
}

static int xhci_control_request(usb_controller_t* controller,
                                uint8_t request_type, uint8_t request,
                                uint16_t value, uint16_t index,
                                void* data, uint16_t length,
                                uint32_t* actual_length)
{
    xhci_trb_t trbs[3];
    uint8_t* buffer = controller->transfer_buffer;
    uint64_t setup;
    uint32_t transferred = 0;
    uint32_t count = length ? 3 : 2;

    if (!controller->slot_id || !buffer ||
        (length && (!data || length > FRAME_SIZE)))
        return 0;
    setup = (uint64_t)request_type | ((uint64_t)request << 8) |
            ((uint64_t)value << 16) | ((uint64_t)index << 32) |
            ((uint64_t)length << 48);
    trbs[0].parameter = setup;
    trbs[0].status = 8;
    trbs[0].control = (XHCI_TRB_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT) |
                      (1U << 6) |
                      (length ? ((request_type & 0x80) ? 3U : 2U) << 16 : 0) |
                      (1U << 4);
    if (length) {
        if (!(request_type & 0x80)) memcpy(buffer, data, length);
        trbs[1].parameter = (uint64_t)(uintptr_t)buffer;
        trbs[1].status = length;
        trbs[1].control = (XHCI_TRB_DATA_STAGE << XHCI_TRB_TYPE_SHIFT) |
                          (1U << 4) |
                          ((request_type & 0x80) ? (1U << 16) : 0);
        trbs[2].parameter = 0;
        trbs[2].status = 0;
        trbs[2].control = (XHCI_TRB_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) |
                          (1U << 5) |
                          ((request_type & 0x80) ? 0 : (1U << 16));
    } else {
        trbs[1].parameter = 0;
        trbs[1].status = 0;
        trbs[1].control = (XHCI_TRB_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT) |
                          (1U << 5) | (1U << 16);
    }
    if (!xhci_transfer(controller, controller->slot_id, 1,
                       trbs, count, &transferred))
        return 0;
    if (length && (request_type & 0x80)) {
        if (transferred > length) return 0;
        if (data != buffer)
            memcpy(data, buffer, length - transferred);
        if (actual_length) *actual_length = length - transferred;
    } else if (actual_length) {
        *actual_length = length;
    }
    return 1;
}

static int xhci_configure_endpoint(usb_controller_t* controller,
                                   uint8_t endpoint_id,
                                   uint8_t endpoint_type,
                                   uint16_t max_packet_size,
                                   uint8_t interval)
{
    uint8_t* input = (uint8_t*)xhci_alloc_dma_page();
    uint32_t stride = controller->context_size;
    uint32_t* control;
    uint32_t* slot;
    uint32_t* endpoint;
    uint8_t* output = controller->device_context;
    xhci_trb_t* ring;
    int result;
    if (!input || endpoint_id < 2 || endpoint_id >= 32) {
        if (input) pmm_free_frame(input);
        return 0;
    }
    ring = (xhci_trb_t*)xhci_alloc_dma_page();
    if (!ring) {
        pmm_free_frame(input);
        return 0;
    }
    controller->transfer_rings[endpoint_id] = ring;
    controller->transfer_index[endpoint_id] = 0;
    controller->transfer_cycle[endpoint_id] = 1;
    control = xhci_context(input, stride, 0);
    slot = xhci_context(input, stride, 1);
    endpoint = xhci_context(input, stride, endpoint_id + 1);
    memcpy(slot, xhci_context(output, stride, 0), stride);
    memcpy(endpoint, xhci_context(output, stride, endpoint_id), stride);
    *control = 0;
    control[1] = 1U | (1U << endpoint_id);
    slot[0] &= ~(0x1FU << 27);
    slot[0] |= (uint32_t)endpoint_id << 27;
    endpoint[0] = (XHCI_ENDPOINT_CERR << 1) |
                  ((uint32_t)interval << 16);
    endpoint[1] = (3U << 1) | ((uint32_t)endpoint_type << 3) |
                  ((uint32_t)max_packet_size << 16);
    endpoint[2] = (uint32_t)(uintptr_t)ring | 1U;
    endpoint[3] = (uint32_t)((uint64_t)(uintptr_t)ring >> 32);
    endpoint[4] = 0;
    result = xhci_command_trb(controller, XHCI_TRB_CONFIGURE_ENDPOINT,
                              controller->slot_id,
                              (uint64_t)(uintptr_t)input, 0, NULL);
    if (result) {
        xhci_release_context(controller, input);
    } else {
        xhci_track_context(controller, input);
    }
    return result;
}

static uint32_t usb_read_be32(const uint8_t* bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

static uint8_t xhci_interrupt_interval(uint8_t speed, uint8_t interval)
{
    uint32_t microframes;
    uint8_t exponent = 0;

    if (!interval) return 0;
    if (speed >= 3) return interval > 16 ? 15 : (uint8_t)(interval - 1);
    microframes = (uint32_t)interval * 8U;
    while (exponent < 15 && (1U << exponent) < microframes)
        exponent++;
    return exponent;
}

static uint32_t usb_read_le32(const uint8_t* bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void usb_write_le32(uint8_t* bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void usb_write_be32(uint8_t* bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static int xhci_bulk_transfer(usb_controller_t* controller,
                              uint8_t endpoint_id, void* buffer,
                              uint32_t length, uint32_t* actual_length)
{
    xhci_trb_t trb;
    uint32_t residual = 0;
    if (!buffer || !length || length > FRAME_SIZE) return 0;
    trb.parameter = (uint64_t)(uintptr_t)buffer;
    trb.status = length;
    trb.control = (XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT) | (1U << 5);
    if (!xhci_transfer(controller, controller->slot_id, endpoint_id,
                       &trb, 1, &residual))
        return 0;
    if (residual > length) return 0;
    if (actual_length) *actual_length = length - residual;
    return 1;
}

static int usb_bot_command(usb_controller_t* controller,
                           const uint8_t* cdb, uint8_t cdb_length,
                           uint8_t* data, uint32_t data_length, int data_in)
{
    uint8_t* buffer;
    uint32_t actual;
    uint32_t tag;
    if (!controller || !cdb || !cdb_length || cdb_length > 16 ||
        (!data && data_length))
        return 0;
    buffer = controller->transfer_buffer;
    if (!buffer) return 0;
    tag = ++controller->bot_tag;
    if (!tag) tag = ++controller->bot_tag;
    if (!buffer || !cdb || !cdb_length || cdb_length > 16 ||
        data_length > FRAME_SIZE)
        return 0;
    memset(buffer, 0, 31);
    usb_write_le32(buffer, 0x43425355U);
    usb_write_le32(buffer + 4, tag);
    usb_write_le32(buffer + 8, data_length);
    buffer[12] = data_in ? 0x80 : 0;
    buffer[13] = 0;
    buffer[14] = cdb_length;
    memcpy(buffer + 15, cdb, cdb_length);
    if (!xhci_bulk_transfer(controller, controller->bulk_out_endpoint,
                            buffer, 31, &actual) || actual != 31)
        return 0;
    if (data_length) {
        if (!data || (!data_in && data != buffer))
            memcpy(buffer, data, data_length);
        if (!xhci_bulk_transfer(controller,
                                data_in ? controller->bulk_in_endpoint :
                                          controller->bulk_out_endpoint,
                                buffer, data_length, &actual) ||
            actual != data_length)
            return 0;
        if (data_in && data != buffer) memcpy(data, buffer, data_length);
    }
    memset(buffer, 0, 13);
    if (!xhci_bulk_transfer(controller, controller->bulk_in_endpoint,
                            buffer, 13, &actual) || actual != 13 ||
        usb_read_le32(buffer) != 0x53425355U ||
        usb_read_le32(buffer + 4) != tag ||
        usb_read_le32(buffer + 8) != 0 ||
        buffer[12] != 0)
        return 0;
    return 1;
}

static int usb_storage_transfer(usb_controller_t* controller, uint64_t lba,
                                uint8_t* buffer, int write)
{
    uint8_t cdb[10] = { 0 };
    if (!controller || !controller->storage_ready || !buffer ||
        lba >= controller->sector_count || lba > 0xFFFFFFFFULL)
        return 0;
    cdb[0] = write ? 0x2A : 0x28;
    usb_write_be32(cdb + 2, (uint32_t)lba);
    cdb[7] = 0;
    cdb[8] = 1;
    return usb_bot_command(controller, cdb, sizeof(cdb), buffer,
                           USB_STORAGE_SECTOR_SIZE, !write);
}

static int usb_block_read(void* context, uint64_t lba, uint8_t* buffer)
{
    return usb_storage_transfer((usb_controller_t*)context, lba, buffer, 0);
}

static int usb_block_write(void* context, uint64_t lba,
                           const uint8_t* buffer)
{
    return usb_storage_transfer((usb_controller_t*)context, lba,
                                (uint8_t*)buffer, 1);
}

static int usb_block_flush(void* context)
{
    usb_controller_t* controller = (usb_controller_t*)context;
    const uint8_t cdb[10] = { 0x35 };
    return controller && controller->storage_ready &&
           usb_bot_command(controller, cdb, sizeof(cdb), NULL, 0, 0);
}

static int usb_partition_read(void* context, uint64_t lba, uint8_t* buffer)
{
    usb_partition_t* partition = (usb_partition_t*)context;
    if (!partition || !partition->registered ||
        lba >= partition->sector_count ||
        partition->first_lba > UINT64_MAX - lba)
        return 0;
    return usb_storage_transfer(partition->controller,
                                partition->first_lba + lba, buffer, 0);
}

static int usb_partition_write(void* context, uint64_t lba,
                               const uint8_t* buffer)
{
    usb_partition_t* partition = (usb_partition_t*)context;
    if (!partition || !partition->registered ||
        lba >= partition->sector_count ||
        partition->first_lba > UINT64_MAX - lba)
        return 0;
    return usb_storage_transfer(partition->controller,
                                partition->first_lba + lba,
                                (uint8_t*)buffer, 1);
}

static int usb_partition_flush(void* context)
{
    usb_partition_t* partition = (usb_partition_t*)context;
    return partition && partition->registered &&
           usb_block_flush(partition->controller);
}

static int usb_partition_add(usb_controller_t* controller,
                             uint64_t first_lba, uint64_t sector_count)
{
    static const block_device_ops_t ops = {
        .read = usb_partition_read,
        .write = usb_partition_write,
        .flush = usb_partition_flush
    };
    usb_partition_t* partition;
    int controller_index = (int)(controller - controllers);
    if (controller_index < 0 || controller_index >= 8 ||
        !sector_count || first_lba >= controller->sector_count ||
        sector_count > controller->sector_count - first_lba ||
        controller->partition_count >= 8)
        return 0;
    for (int i = 0; i < controller->partition_count; i++) {
        usb_partition_t* existing = &partitions[controller_index][i];
        uint64_t existing_end = existing->first_lba + existing->sector_count;
        uint64_t new_end = first_lba + sector_count;
        if (first_lba < existing_end && existing->first_lba < new_end)
            return 0;
    }
    partition = &partitions[controller_index][controller->partition_count];
    memset(partition, 0, sizeof(*partition));
    partition->controller = controller;
    partition->first_lba = first_lba;
    partition->sector_count = sector_count;
    partition->name[0] = 'u';
    partition->name[1] = 's';
    partition->name[2] = 'b';
    partition->name[3] = (char)('0' + controller_index);
    partition->name[4] = 'p';
    partition->name[5] = (char)('1' + controller->partition_count);
    partition->name[6] = 0;
    if (!block_register_device(partition->name, sector_count, &ops,
                               partition)) {
        memset(partition, 0, sizeof(*partition));
        return 0;
    }
    partition->registered = 1;
    controller->partition_count++;
    return 1;
}

static int usb_partition_remove_all(usb_controller_t* controller)
{
    int controller_index = (int)(controller - controllers);
    if (controller_index < 0 || controller_index >= 8) return 0;
    while (controller->partition_count > 0) {
        usb_partition_t* partition =
            &partitions[controller_index][controller->partition_count - 1];
        if (partition->registered)
            vfs_forget_device(partition->name);
        if (partition->registered &&
            !block_unregister_device(partition->name)) {
            serial_puts("[usb] ERROR: cannot unregister partition block device\n");
            return 0;
        }
        memset(partition, 0, sizeof(*partition));
        controller->partition_count--;
    }
    return 1;
}

static int usb_scan_gpt(usb_controller_t* controller)
{
    uint8_t sector[BLOCK_SECTOR_SIZE];
    uint8_t header[BLOCK_SECTOR_SIZE];
    uint64_t table_lba, first_usable, last_usable;
    uint32_t header_size, header_crc, entry_count, entry_size, table_crc;
    uint32_t table_bytes, sectors;
    if (!usb_block_read(controller, 1, sector) ||
        memcmp(sector, "EFI PART", 8) != 0)
        return 0;
    header_size = usb_read_le32(sector + 12);
    header_crc = usb_read_le32(sector + 16);
    table_lba = usb_read_le64(sector + 72);
    entry_count = usb_read_le32(sector + 80);
    entry_size = usb_read_le32(sector + 84);
    table_crc = usb_read_le32(sector + 88);
    first_usable = usb_read_le64(sector + 40);
    last_usable = usb_read_le64(sector + 48);
    if (header_size < 92 || header_size > BLOCK_SECTOR_SIZE ||
        !entry_count || entry_count > 128 || entry_size != 128 ||
        usb_read_le64(sector + 24) != 1 ||
        usb_read_le64(sector + 32) >= controller->sector_count ||
        table_lba < 2 ||
        first_usable > last_usable ||
        last_usable >= controller->sector_count ||
        table_lba >= controller->sector_count)
        return 0;
    memcpy(header, sector, BLOCK_SECTOR_SIZE);
    memset(header + 16, 0, 4);
    if (usb_crc32(header, header_size) != header_crc)
        return 0;
    table_bytes = entry_count * entry_size;
    sectors = (table_bytes + BLOCK_SECTOR_SIZE - 1) / BLOCK_SECTOR_SIZE;
    if (sectors > controller->sector_count - table_lba)
        return 0;
    if (table_lba + sectors > first_usable)
        return 0;
    uint32_t crc = 0xFFFFFFFFU;
    uint32_t remaining = table_bytes;
    for (uint32_t i = 0; i < sectors; i++) {
        if (!usb_block_read(controller, table_lba + i, sector))
            return 0;
        uint32_t count = remaining > BLOCK_SECTOR_SIZE ?
                         BLOCK_SECTOR_SIZE : remaining;
        for (uint32_t j = 0; j < count; j++) {
            crc ^= sector[j];
            for (int bit = 0; bit < 8; bit++)
                crc = (crc >> 1) ^
                      (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
        remaining -= count;
    }
    if (~crc != table_crc) return 0;
    for (uint32_t i = 0; i < entry_count &&
         controller->partition_count < 8; i++) {
        uint64_t entry_lba = table_lba +
                             ((uint64_t)i * entry_size) / BLOCK_SECTOR_SIZE;
        uint32_t entry_offset = (i * entry_size) % BLOCK_SECTOR_SIZE;
        uint8_t entry[128];
        if (entry_offset + sizeof(entry) > BLOCK_SECTOR_SIZE ||
            !usb_block_read(controller, entry_lba, sector))
            return 0;
        memcpy(entry, sector + entry_offset, sizeof(entry));
        int nonzero_type = 0;
        for (int byte = 0; byte < 16; byte++)
            nonzero_type |= entry[byte] != 0;
        if (!nonzero_type) continue;
        uint64_t first = usb_read_le64(entry + 32);
        uint64_t last = usb_read_le64(entry + 40);
        if (first < first_usable || last > last_usable || first > last)
            continue;
        (void)usb_partition_add(controller, first, last - first + 1);
    }
    return 1;
}

static void usb_scan_partitions(usb_controller_t* controller)
{
    uint8_t sector[BLOCK_SECTOR_SIZE];
    int protective_gpt = 0;
    controller->partition_count = 0;
    if (!controller->storage_ready ||
        !usb_block_read(controller, 0, sector) ||
        usb_read_le16(sector + 510) != 0xAA55)
        return;
    for (int i = 0; i < 4; i++) {
        const uint8_t* entry = sector + 446 + i * 16;
        uint8_t type = entry[4];
        uint64_t first = usb_read_le32(entry + 8);
        uint64_t count = usb_read_le32(entry + 12);
        if (type == 0xEE) protective_gpt = 1;
        if (!type || type == 0xEE || !count) continue;
        (void)usb_partition_add(controller, first, count);
    }
    if (protective_gpt) {
        if (!usb_partition_remove_all(controller)) return;
        if (!usb_scan_gpt(controller))
            serial_puts("[usb] GPT partition table is invalid or unsupported\n");
    }
}

static int xhci_set_address(usb_controller_t* controller, int port,
                            uint8_t speed)
{
    uint8_t* input = (uint8_t*)xhci_alloc_dma_page();
    xhci_trb_t* ep0_ring = (xhci_trb_t*)xhci_alloc_dma_page();
    uint32_t stride = controller->context_size;
    uint32_t* control;
    uint32_t* slot;
    uint32_t* endpoint;
    uint8_t slot_id = 0;
    uint16_t packet_size;
    int result = 0;
    if (controller->slot_id) goto done;
    if (!input || !ep0_ring) goto done;
    if (!xhci_command(controller, 9, 0, &slot_id) || !slot_id ||
        slot_id > controller->max_slots)
        goto done;
    controller->slot_id = slot_id;
    controller->slot_port = port;
    if ((uint64_t)controller->doorbell_offset +
        (uint64_t)slot_id * 4 + 4 > controller->bar_size)
        goto disable_slot;
    controller->device_context = (uint8_t*)xhci_alloc_dma_page();
    controller->transfer_buffer = (uint8_t*)xhci_alloc_dma_page();
    if (!controller->device_context || !controller->transfer_buffer)
        goto disable_slot;
    controller->device_context_array[slot_id] =
        (uint64_t)(uintptr_t)controller->device_context;
    packet_size = speed == 3 ? 64 : (speed >= 4 ? 512 : 8);
    controller->ep0_packet_size = packet_size;
    controller->transfer_rings[1] = ep0_ring;
    controller->transfer_index[1] = 0;
    controller->transfer_cycle[1] = 1;

    control = xhci_context(input, stride, 0);
    slot = xhci_context(input, stride, 1);
    endpoint = xhci_context(input, stride, 2);
    control[1] = 3;
    slot[0] = ((uint32_t)speed << 20) | (1U << 27);
    slot[1] = (uint32_t)(port + 1) << 16;
    endpoint[0] = XHCI_ENDPOINT_CERR << 1;
    endpoint[1] = (3U << 1) | (4U << 3) |
                  ((uint32_t)packet_size << 16);
    endpoint[2] = (uint32_t)(uintptr_t)ep0_ring | 1U;
    endpoint[3] = (uint32_t)((uint64_t)(uintptr_t)ep0_ring >> 32);
    endpoint[4] = 8;
    xhci_track_context(controller, input);
    result = xhci_command_trb(controller, XHCI_TRB_ADDRESS_DEVICE,
                              slot_id, (uint64_t)(uintptr_t)input, 0, NULL);
    if (!result) goto disable_slot;
    xhci_release_context(controller, input);
    input = NULL;
    return 1;

disable_slot:
    if (!xhci_command(controller, XHCI_TRB_DISABLE_SLOT, slot_id, NULL))
        goto done;
    if (xhci_context_is_pending(controller, input)) input = NULL;
    xhci_release_pending_contexts(controller);
    controller->device_context_array[slot_id] = 0;
    controller->slot_id = 0;
    if (controller->device_context) pmm_free_frame(controller->device_context);
    if (controller->transfer_buffer) pmm_free_frame(controller->transfer_buffer);
    if (ep0_ring) pmm_free_frame(ep0_ring);
    controller->device_context = NULL;
    controller->transfer_buffer = NULL;
    controller->transfer_rings[1] = NULL;
    controller->slot_id = 0;
    controller->slot_port = -1;
done:
    if (input && !xhci_context_is_pending(controller, input))
        pmm_free_frame(input);
    if (ep0_ring && controller->transfer_rings[1] != ep0_ring)
        pmm_free_frame(ep0_ring);
    return 0;
}

static int xhci_update_ep0_packet_size(usb_controller_t* controller,
                                       uint16_t packet_size)
{
    uint8_t* input = (uint8_t*)xhci_alloc_dma_page();
    uint32_t* control;
    uint32_t* endpoint;
    uint32_t stride = controller->context_size;
    int result;
    if (!input || packet_size == 0) {
        if (input) pmm_free_frame(input);
        return 0;
    }
    control = xhci_context(input, stride, 0);
    endpoint = xhci_context(input, stride, 2);
    memcpy(endpoint, xhci_context(controller->device_context, stride, 1),
           stride);
    endpoint[1] = (endpoint[1] & 0x0000FFFFU) |
                  ((uint32_t)packet_size << 16);
    control[1] = 1U << 1;
    result = xhci_command_trb(controller, XHCI_TRB_EVALUATE_CONTEXT,
                              controller->slot_id,
                              (uint64_t)(uintptr_t)input, 0, NULL);
    if (result)
        xhci_release_context(controller, input);
    else
        xhci_track_context(controller, input);
    if (result) controller->ep0_packet_size = packet_size;
    return result;
}

static int xhci_release_device(usb_controller_t* controller)
{
    uint8_t slot_id = controller->slot_id;
    if (!slot_id) return 1;
    if (!xhci_command(controller, XHCI_TRB_DISABLE_SLOT, slot_id, NULL))
        return 0;
    xhci_release_pending_contexts(controller);
    if (!usb_partition_remove_all(controller)) return 0;
    if (controller->storage_registered) {
        vfs_forget_device(controller->block_name);
        if (!block_unregister_device(controller->block_name)) {
            serial_puts("[usb] ERROR: cannot unregister storage block device\n");
            return 0;
        }
        controller->storage_registered = 0;
    }
    controller->device_context_array[slot_id] = 0;
    for (int endpoint = 1; endpoint < 32; endpoint++) {
        if (controller->transfer_rings[endpoint]) {
            pmm_free_frame(controller->transfer_rings[endpoint]);
            controller->transfer_rings[endpoint] = NULL;
        }
    }
    if (controller->device_context) pmm_free_frame(controller->device_context);
    if (controller->transfer_buffer) pmm_free_frame(controller->transfer_buffer);
    if (controller->hid_buffer) pmm_free_frame(controller->hid_buffer);
    controller->device_context = NULL;
    controller->transfer_buffer = NULL;
    controller->hid_buffer = NULL;
    controller->slot_id = 0;
    controller->storage_ready = 0;
    controller->media_online = 0;
    controller->sector_count = 0;
    controller->slot_port = -1;
    controller->partition_count = 0;
    controller->hid_endpoint = 0;
    controller->hid_protocol = 0;
    controller->hid_interface = 0;
    controller->hid_packet_size = 0;
    controller->hid_pending = 0;
    controller->hid_failed = 0;
    memset(controller->hid_previous, 0, sizeof(controller->hid_previous));
    controller->slot_id = 0;
    return 1;
}

static int xhci_enumerate_mass_storage(usb_controller_t* controller,
                                       int port, uint32_t port_status)
{
    uint8_t speed = (uint8_t)((port_status >> XHCI_PORTSC_SPEED_SHIFT) &
                              XHCI_PORTSC_SPEED_MASK);
    uint8_t device_descriptor[18];
    uint8_t* configuration;
    uint8_t config_header[9];
    uint8_t configuration_value = 0;
    uint8_t in_address = 0, out_address = 0;
    uint16_t in_packet = 0, out_packet = 0;
    uint8_t current_class = 0, current_subclass = 0, current_protocol = 0;
    uint8_t current_interface = 0;
    uint8_t hid_address = 0, hid_interface = 0, hid_protocol = 0;
    uint8_t hid_interval = 0;
    uint16_t hid_packet = 0;
    uint8_t configuration_length;
    uint32_t descriptor_size;
    uint32_t actual;
    uint8_t cdb[16] = { 0x25 };
    uint8_t capacity[8];
    const char* failure_stage = "port reset or address";
    block_device_ops_t ops = {
        .read = usb_block_read,
        .write = usb_block_write,
        .flush = usb_block_flush
    };

    if (controller->slot_id) {
        serial_puts("[usb] xHCI slot already occupied\n");
        return 0;
    }
    if (speed < 1 || speed > 4) {
        serial_puts("[usb] unsupported xHCI device speed\n");
        return 0;
    }
    if (!usb_port_reset((int)(controller - controllers), port)) {
        serial_puts("[usb] ERROR: xHCI port reset failed\n");
        return 0;
    }
    serial_puts("[usb] xHCI port reset complete\n");
    if (!xhci_set_address(controller, port, speed)) {
        serial_puts("[usb] ERROR: xHCI address assignment failed\n");
        return 0;
    }
    serial_puts("[usb] xHCI address assigned\n");

    failure_stage = "device descriptor";
    if (!xhci_control_request(controller, 0x80, 6, 0x0100, 0,
                              device_descriptor,
                              sizeof(device_descriptor), &actual) ||
        actual < sizeof(device_descriptor))
        goto fail;
    failure_stage = "endpoint-zero packet size";
    if ((speed == 4 && device_descriptor[7] != 9) ||
        (speed == 3 && device_descriptor[7] != 64) ||
        (speed == 2 && device_descriptor[7] != 8) ||
        (speed == 1 && (device_descriptor[7] < 8 ||
                        device_descriptor[7] > 64)) ||
        !xhci_update_ep0_packet_size(
            controller, speed == 4 ? 512 : device_descriptor[7]))
        goto fail;
    failure_stage = "configuration descriptor";
    if (!xhci_control_request(controller, 0x80, 6, 0x0200, 0,
                              config_header, sizeof(config_header), &actual) ||
        actual < sizeof(config_header))
        goto fail;
    descriptor_size = (uint32_t)config_header[2] |
                      ((uint32_t)config_header[3] << 8);
    configuration_length = config_header[5];
    if (descriptor_size < 9 || descriptor_size > FRAME_SIZE ||
        config_header[1] != 2 || !configuration_length)
        goto fail;
    configuration = controller->transfer_buffer;
    if (!xhci_control_request(controller, 0x80, 6, 0x0200, 0,
                              configuration, (uint16_t)descriptor_size,
                              &actual) || actual < 9)
        goto fail;

    for (uint32_t offset = 0; offset + 2 <= actual;) {
        uint8_t descriptor_length = configuration[offset];
        uint8_t descriptor_type = configuration[offset + 1];
        if (descriptor_length < 2 || descriptor_length > actual - offset)
            goto fail;
        if (descriptor_type == 4 && descriptor_length >= 9) {
            current_class = configuration[offset + 5];
            current_subclass = configuration[offset + 6];
            current_protocol = configuration[offset + 7];
            current_interface = configuration[offset + 2];
            if (current_class == XHCI_USB_CLASS_HID &&
                current_subclass == XHCI_USB_HID_SUBCLASS_BOOT &&
                (current_protocol == XHCI_USB_HID_PROTOCOL_KEYBOARD ||
                 current_protocol == XHCI_USB_HID_PROTOCOL_MOUSE)) {
                hid_interface = current_interface;
                hid_protocol = current_protocol;
            }
        } else if (descriptor_type == 5 && descriptor_length >= 7 &&
                   current_class == XHCI_USB_CLASS_MASS_STORAGE &&
                   (configuration[offset + 3] & 3U) == 2) {
            uint8_t address = configuration[offset + 2];
            uint16_t packet = (uint16_t)configuration[offset + 4] |
                              ((uint16_t)configuration[offset + 5] << 8);
            if (address & 0x80) {
                in_address = address;
                in_packet = packet & 0x07FFU;
            } else {
                out_address = address;
                out_packet = packet & 0x07FFU;
            }
        } else if (descriptor_type == 5 && descriptor_length >= 7 &&
                   current_class == XHCI_USB_CLASS_HID &&
                   current_subclass == XHCI_USB_HID_SUBCLASS_BOOT &&
                   (current_protocol == XHCI_USB_HID_PROTOCOL_KEYBOARD ||
                    current_protocol == XHCI_USB_HID_PROTOCOL_MOUSE) &&
                   (configuration[offset + 3] & 3U) == 3 &&
                   (configuration[offset + 2] & 0x80U)) {
            hid_address = configuration[offset + 2];
            hid_packet = (uint16_t)(
                ((uint16_t)configuration[offset + 5] << 8) |
                configuration[offset + 4]) & 0x07FFU;
            hid_interval = configuration[offset + 6];
        } else if (descriptor_type == 2 && descriptor_length >= 9) {
            configuration_value = configuration[offset + 5];
        }
        offset += descriptor_length;
    }
    if (!configuration_value) goto fail;
    if (!xhci_control_request(controller, 0x00, 9, configuration_value,
                              0, NULL, 0, NULL))
        goto fail;

    if (hid_address && hid_packet && hid_interval && hid_protocol) {
        uint8_t endpoint_id =
            (uint8_t)(((hid_address & 0x0FU) * 2U) + 1U);
        failure_stage = "HID endpoint setup";
        if ((hid_address & 0x0FU) == 0 ||
            hid_packet > 64 ||
            hid_packet > FRAME_SIZE || endpoint_id >= 32)
            goto fail;
        controller->hid_buffer = (uint8_t*)xhci_alloc_dma_page();
        if (!controller->hid_buffer) goto fail;
        controller->hid_endpoint = hid_address;
        controller->hid_protocol = hid_protocol;
        controller->hid_interface = hid_interface;
        controller->hid_packet_size = (uint8_t)hid_packet;
        if (!xhci_configure_endpoint(
                controller, endpoint_id, 7, hid_packet,
                xhci_interrupt_interval(speed, hid_interval)) ||
            !xhci_control_request(controller, 0x21, 0x0B, 0,
                                  hid_interface, NULL, 0, NULL) ||
            !xhci_control_request(controller, 0x21, 0x0A, 0,
                                  hid_interface, NULL, 0, NULL))
            goto fail;
        if (!xhci_hid_submit(controller)) goto fail;
        serial_puts(hid_protocol == XHCI_USB_HID_PROTOCOL_KEYBOARD ?
                    "[usb] USB HID boot keyboard ready\n" :
                    "[usb] USB HID boot mouse ready\n");
        return 1;
    }

    if (!in_address || !out_address || !in_packet || !out_packet ||
        (in_address & 0x0FU) == 0 || (out_address & 0x0FU) == 0 ||
        in_packet > (speed == 4 ? 1024 : speed == 3 ? 512 : 64) ||
        out_packet > (speed == 4 ? 1024 : speed == 3 ? 512 : 64))
        goto fail;

    failure_stage = "bulk endpoint setup";
    controller->bulk_in_endpoint =
        (uint8_t)(((in_address & 0x0FU) * 2U) + 1U);
    controller->bulk_out_endpoint =
        (uint8_t)((out_address & 0x0FU) * 2U);
    controller->bulk_in_packet_size = in_packet;
    controller->bulk_out_packet_size = out_packet;
    if (!xhci_configure_endpoint(controller, controller->bulk_in_endpoint,
                                 6, in_packet, 0) ||
        !xhci_configure_endpoint(controller, controller->bulk_out_endpoint,
                                 2, out_packet, 0))
        goto fail;

    failure_stage = "BOT/SCSI commands";
    if (!usb_bot_command(controller, (const uint8_t[]){ 0x12, 0, 0, 0, 36, 0 },
                         6, controller->transfer_buffer, 36, 1) ||
        !usb_bot_command(controller, (const uint8_t[]){ 0, 0, 0, 0, 0, 0 }, 6,
                         NULL, 0, 0) ||
        !usb_bot_command(controller, cdb, sizeof(cdb), capacity,
                         sizeof(capacity), 1))
        goto fail;
    failure_stage = "SCSI capacity validation";
    controller->sector_count = (uint64_t)usb_read_be32(capacity) + 1;
    if (usb_read_be32(capacity + 4) != USB_STORAGE_SECTOR_SIZE ||
        !controller->sector_count ||
        controller->sector_count > 0x100000000ULL)
        goto fail;
    controller->storage_ready = 1;
    controller->media_online = 1;
    controller->block_name[0] = 'u';
    controller->block_name[1] = 's';
    controller->block_name[2] = 'b';
    controller->block_name[3] = (char)('0' +
        (controller - controllers));
    controller->block_name[4] = 0;
    failure_stage = "block-device registration";
    if (!block_register_device(controller->block_name,
                               controller->sector_count,
                               &ops, controller))
        goto fail;
    controller->storage_registered = 1;
    usb_scan_partitions(controller);
    return 1;

fail:
    serial_puts("[usb] ERROR: enumeration failed during ");
    serial_puts(failure_stage);
    serial_puts("\n");
    (void)xhci_release_device(controller);
    return 0;
}

static int xhci_handoff_firmware(usb_controller_t* controller)
{
    uint32_t extended = usb_mmio_read(controller->mmio, 0x10) >> 16;
    uint32_t visited = 0;

    while (extended && visited++ < 64) {
        uint32_t header;
        uint32_t offset = extended * 4;
        if (offset > controller->bar_size - sizeof(uint32_t)) return 0;
        header = usb_mmio_read(controller->mmio, offset);
        if ((header & 0xFF) == 1) {
            uint32_t ownership = usb_mmio_read(controller->mmio, offset);
            if (ownership & (1U << 16)) {
                usb_mmio_write(controller->mmio, offset,
                               ownership | (1U << 24));
                for (uint32_t i = 0; i < 100000000; i++) {
                    ownership = usb_mmio_read(controller->mmio, offset);
                    if (!(ownership & (1U << 16))) break;
                }
                if (ownership & (1U << 16)) return 0;
            }
        }
        extended = (header >> 8) & 0xFF;
    }
    return visited <= 64;
}

static int ehci_reset(volatile uint8_t* base)
{
    uint32_t capability_length = *(volatile uint8_t*)base;
    uint32_t command;
    uint32_t status;
    if (capability_length < 0x10 || capability_length > 0x40) return 0;
    uint32_t operational = capability_length;
    command = usb_mmio_read(base, operational + 0x00);
    usb_mmio_write(base, operational + 0x00, command & ~1U);
    for (uint32_t i = 0; i < 100000; i++) {
        status = usb_mmio_read(base, operational + 0x04);
        if (status & (1U << 12)) break;
    }
    usb_mmio_write(base, operational + 0x04, 0x3FU);
    usb_mmio_write(base, operational + 0x00, 2U);
    for (uint32_t i = 0; i < 100000; i++)
        if (!(usb_mmio_read(base, operational + 0x00) & 2U)) break;
    if (usb_mmio_read(base, operational + 0x00) & 2U) return 0;
    usb_mmio_write(base, operational + 0x50, 1U);
    command = usb_mmio_read(base, operational + 0x00);
    usb_mmio_write(base, operational + 0x00, command | 1U);
    for (uint32_t i = 0; i < 100000; i++) {
        if (!(usb_mmio_read(base, operational + 0x04) & (1U << 12))) break;
    }
    if (usb_mmio_read(base, operational + 0x04) & (1U << 12)) return 0;
    return 1;
}

static int xhci_start(usb_controller_t* controller)
{
    volatile uint8_t* base = controller->mmio;
    uint32_t capability_length = *(volatile uint8_t*)base;
    uint32_t hcsparams1;
    uint32_t command;
    uint32_t page_size;
    uint64_t command_ring_address;
    uint32_t port_registers_offset;
    uint32_t available_ports;
    uint32_t operational;

    if (capability_length < 0x20 ||
        capability_length > controller->bar_size - 0x400)
        return 0;
    controller->operational = capability_length;
    operational = controller->operational;
    hcsparams1 = usb_mmio_read(base, 0x04);
    controller->max_slots = hcsparams1 & 0xFF;
    if (controller->max_slots > XHCI_MAX_SLOTS)
        controller->max_slots = XHCI_MAX_SLOTS;
    controller->port_count = (int)((hcsparams1 >> 24) & 0xFF);
    if (!controller->max_slots || !controller->port_count) return 0;
    port_registers_offset = operational + 0x400;
    if (port_registers_offset > controller->bar_size) return 0;
    available_ports =
        (uint32_t)((controller->bar_size - port_registers_offset) / 0x10);
    if ((uint32_t)controller->port_count > available_ports)
        controller->port_count = (int)available_ports;
    if (!controller->port_count) return 0;

    controller->doorbell_offset = usb_mmio_read(base, 0x14) & ~3U;
    controller->runtime_offset = usb_mmio_read(base, 0x18) & ~0x1FU;
    if (controller->doorbell_offset > controller->bar_size - 4 ||
        controller->runtime_offset > controller->bar_size - 0x40)
        return 0;
    if (!xhci_handoff_firmware(controller)) return 0;
    controller->context_size =
        (usb_mmio_read(base, 0x10) & (1U << 2)) ? 64U : 32U;

    command = usb_mmio_read(base, operational);
    usb_mmio_write(base, operational, command & ~1U);
    if (!xhci_wait_status(controller, 1U, 1U)) return 0;
    usb_mmio_write(base, operational, (command & ~1U) | 2U);
    if (!xhci_wait_status(controller, 2U, 0U)) return 0;
    if (!xhci_wait_status(controller, 1U << 11, 0U)) return 0;
    page_size = usb_mmio_read(base, operational + 0x08);
    if (!(page_size & 1U)) return 0;

    controller->command_ring = (xhci_trb_t*)xhci_alloc_dma_page();
    controller->event_ring = (xhci_trb_t*)xhci_alloc_dma_page();
    controller->device_context_array =
        (uint64_t*)xhci_alloc_dma_page();
    controller->event_ring_segment_table =
        (uint64_t*)xhci_alloc_dma_page();
    if (!controller->command_ring || !controller->event_ring ||
        !controller->device_context_array ||
        !controller->event_ring_segment_table) {
        xhci_release_rings(controller);
        return 0;
    }

    controller->command_index = 0;
    controller->event_index = 0;
    controller->command_cycle = 1;
    controller->event_cycle = 1;
    controller->command_ring[XHCI_TRBS_PER_RING - 1].parameter =
        (uint64_t)(uintptr_t)controller->command_ring;
    controller->command_ring[XHCI_TRBS_PER_RING - 1].control =
        (6U << 10) | (1U << 1) | 1U;
    controller->event_ring_segment_table[0] =
        (uint64_t)(uintptr_t)controller->event_ring;
    controller->event_ring_segment_table[1] = XHCI_TRBS_PER_RING;
    command_ring_address = (uint64_t)(uintptr_t)controller->command_ring | 1U;

    usb_mmio_write64(base, operational + 0x18, command_ring_address);
    usb_mmio_write64(base, operational + 0x30,
                     (uint64_t)(uintptr_t)controller->device_context_array);
    usb_mmio_write(base, operational + 0x38, controller->max_slots);

    uint32_t interrupter = (uint32_t)controller->runtime_offset + 0x20;
    usb_mmio_write(base, interrupter + 0x00, 0);
    usb_mmio_write(base, interrupter + 0x08, 1);
    usb_mmio_write64(base, interrupter + 0x10,
                     (uint64_t)(uintptr_t)
                         controller->event_ring_segment_table);
    usb_mmio_write64(base, interrupter + 0x18,
                     (uint64_t)(uintptr_t)controller->event_ring);

    usb_mmio_write(base, operational + 0x04, 0x3FU);
    command = usb_mmio_read(base, operational);
    usb_mmio_write(base, operational, command | 1U);
    if (!xhci_wait_status(controller, 1U, 0U)) {
        if (xhci_halt(controller)) {
            xhci_release_rings(controller);
        } else {
            serial_puts("[usb] ERROR: xHCI did not halt; retaining DMA rings\n");
        }
        return 0;
    }

    {
        uint8_t slot_id = 0;
        controller->initialized = 1;
        if (!xhci_command(controller, XHCI_TRB_ENABLE_SLOT, 0, &slot_id) ||
            slot_id == 0 ||
            !xhci_command(controller, XHCI_TRB_DISABLE_SLOT, slot_id, NULL)) {
            controller->initialized = 0;
            if (xhci_halt(controller)) {
                xhci_release_rings(controller);
            } else {
                serial_puts("[usb] ERROR: xHCI command test failed and controller did not halt; retaining DMA rings\n");
            }
            return 0;
        }
        controller->initialized = 0;
    }
    return 1;
}

static const char* usb_name(uint8_t prog_if)
{
    switch (prog_if) {
        case USB_PROGIF_UHCI: return "UHCI";
        case USB_PROGIF_OHCI: return "OHCI";
        case USB_PROGIF_EHCI: return "EHCI";
        case USB_PROGIF_XHCI: return "xHCI";
        default: return "USB";
    }
}

#ifdef USB_STORAGE_SELFTEST
static int usb_storage_selftest(void)
{
    uint8_t original[USB_STORAGE_SECTOR_SIZE];
    uint8_t pattern[USB_STORAGE_SECTOR_SIZE];
    uint8_t readback[USB_STORAGE_SECTOR_SIZE];
    int tested = 0;
    int passed = 1;

    for (int i = 0; i < controller_total; i++) {
        usb_controller_t* controller = &controllers[i];
        uint64_t lba;
        int restore_ok;

        if (!usb_storage_ready_at(i)) continue;
        tested++;
        if (!controller->sector_count) {
            passed = 0;
            continue;
        }
        lba = controller->sector_count - 1;
        if (!usb_storage_transfer(controller, lba, original, 0)) {
            passed = 0;
            continue;
        }
        for (uint32_t byte = 0; byte < sizeof(pattern); byte++)
            pattern[byte] = (uint8_t)(byte * 37U + i * 13U + 0x5AU);

        if (!usb_storage_transfer(controller, lba, pattern, 1) ||
            !usb_block_flush(controller) ||
            !usb_storage_transfer(controller, lba, readback, 0) ||
            memcmp(pattern, readback, sizeof(pattern)) != 0)
            passed = 0;

        restore_ok = usb_storage_transfer(controller, lba, original, 1) &&
                     usb_block_flush(controller) &&
                     usb_storage_transfer(controller, lba, readback, 0) &&
                     memcmp(original, readback, sizeof(original)) == 0;
        if (!restore_ok) {
            serial_puts("[usb-test] ERROR: failed to restore test sector\n");
            passed = 0;
        }
    }

    if (tested && passed)
        serial_puts("[usb-test] PASS: BOT sector write/read/restore\n");
    else
        serial_puts("[usb-test] FAIL: no tested storage device or I/O error\n");
    return tested && passed;
}
#endif

int usb_init(void)
{
    controller_total = 0;
    memset(connected_state, 0, sizeof(connected_state));
    for (int i = 0; i < pci_device_count() && controller_total < 8; i++) {
        const pci_device_t* device = pci_device_at(i);
        if (!device || device->class_code != PCI_CLASS_SERIAL_BUS ||
            device->subclass != PCI_SUBCLASS_USB) continue;
        if (device->prog_if != USB_PROGIF_UHCI &&
            device->prog_if != USB_PROGIF_EHCI &&
            device->prog_if != USB_PROGIF_XHCI)
            continue;
        usb_controller_t* controller = &controllers[controller_total++];
        memset(controller, 0, sizeof(*controller));
        controller->pci = *device;
        controller->name = usb_name(device->prog_if);
        controller->initialized = pci_enable_device(device, 1);
        controller->slot_port = -1;
        if (controller->initialized && device->prog_if == USB_PROGIF_UHCI &&
            (device->bar[4] & 1)) {
            controller->initialized = uhci_reset((uint16_t)(device->bar[4] & 0xFFFC));
            if (controller->initialized)
                controller->port_count = 2;
        }
        if (controller->initialized && device->prog_if != USB_PROGIF_UHCI) {
            const pci_bar_t* bar = pci_bar_info(device, 0);
            if (!bar || !bar->present ||
                (bar->type != 0 && bar->type != 2) || !bar->base ||
                (bar->base >> 32) != 0) {
                controller->initialized = 0;
            } else {
                controller->mmio = (volatile uint8_t*)pci_map_bar(device, 0);
                controller->bar_size = bar->size;
                if (!controller->mmio || bar->size < 0x1000) {
                    controller->initialized = 0;
                    continue;
                }
                controller->operational = *(volatile uint8_t*)controller->mmio;
                if (device->prog_if == USB_PROGIF_EHCI)
                    controller->initialized = ehci_reset(controller->mmio);
                else if (device->prog_if == USB_PROGIF_XHCI)
                    controller->initialized = xhci_start(controller);
                if (controller->initialized) {
                    if (device->prog_if == USB_PROGIF_EHCI)
                        controller->port_count = (int)(usb_mmio_read(
                            controller->mmio, 4) & 0x0F);
                }
            }
        }
    }
    for (int i = 0; i < controller_total; i++) {
        usb_controller_t* controller = &controllers[i];
        if (!controller->initialized ||
            controller->pci.prog_if != USB_PROGIF_XHCI)
            continue;
        for (int port = 0; port < controller->port_count; port++) {
            uint32_t status = usb_mmio_read(
                controller->mmio,
                controller->operational + 0x400 + (uint32_t)port * 0x10);
            connected_state[i][port] = (uint8_t)((status & XHCI_PORTSC_CCS) != 0);
            if (status & XHCI_PORTSC_CCS) {
                enumeration_attempt_tick[i][port] = g_ticks;
                serial_puts("[usb] xHCI device connected; enumerating\n");
                if (!xhci_enumerate_mass_storage(controller, port, status))
                    serial_puts("[usb] ERROR: xHCI device enumeration failed\n");
                else if (controller->storage_ready)
                    serial_puts("[usb] USB mass-storage device ready\n");
            }
        }
    }
#ifdef USB_STORAGE_SELFTEST
    (void)usb_storage_selftest();
#endif
    return usb_controller_present();
}

int usb_controller_present(void)
{
    for (int i = 0; i < controller_total; i++)
        if (controllers[i].initialized) return 1;
    return 0;
}

const char* usb_controller_name(void)
{
    for (int i = 0; i < controller_total; i++)
        if (controllers[i].initialized) return controllers[i].name;
    return "none";
}

int usb_controller_count(void) { return controller_total; }

const char* usb_controller_name_at(int index)
{
    if (index < 0 || index >= controller_total) return "none";
    return controllers[index].name;
}

int usb_controller_ready_at(int index)
{
    if (index < 0 || index >= controller_total) return 0;
    return controllers[index].initialized;
}

int usb_storage_ready_at(int index)
{
    return index >= 0 && index < controller_total &&
           controllers[index].storage_ready &&
           controllers[index].media_online;
}

uint64_t usb_storage_sector_count_at(int index)
{
    return usb_storage_ready_at(index) ? controllers[index].sector_count : 0;
}

const char* usb_storage_name_at(int index)
{
    return usb_storage_ready_at(index) ? controllers[index].block_name : "";
}

int usb_partition_count_at(int index)
{
    if (!usb_storage_ready_at(index)) return 0;
    return controllers[index].partition_count;
}

int usb_partition_info_at(int index, int partition, char name[16],
                          uint64_t* first_lba, uint64_t* sector_count)
{
    usb_partition_t* entry;
    if (!name || !first_lba || !sector_count ||
        !usb_storage_ready_at(index) || partition < 0 ||
        partition >= controllers[index].partition_count)
        return 0;
    entry = &partitions[index][partition];
    if (!entry->registered) return 0;
    memcpy(name, entry->name, sizeof(entry->name));
    *first_lba = entry->first_lba;
    *sector_count = entry->sector_count;
    return 1;
}

int usb_port_count(int controller)
{
    if (controller < 0 || controller >= controller_total ||
        !controllers[controller].initialized) return 0;
    return controllers[controller].port_count;
}

int usb_port_connected(int controller, int port)
{
    usb_controller_t* device;
    uint32_t status;
    if (controller < 0 || controller >= controller_total) return 0;
    device = &controllers[controller];
    if (!device->initialized || port < 0 || port >= device->port_count) return 0;
    if (device->pci.prog_if == USB_PROGIF_UHCI)
        return (inw((uint16_t)((device->pci.bar[4] & 0xFFFC) +
                               uhci_port_offset(port))) &
                UHCI_PORTSC_CCS) != 0;
    if (device->pci.prog_if == USB_PROGIF_EHCI)
        status = usb_mmio_read(device->mmio, device->operational + 0x44 + port * 4);
    else if (device->pci.prog_if == USB_PROGIF_XHCI)
        status = usb_mmio_read(device->mmio,
                               device->operational + 0x400 + port * 0x10);
    else return 0;
    return (status & 1U) != 0;
}

int usb_port_reset(int controller, int port)
{
    usb_controller_t* device;
    uint32_t offset;
    uint32_t status;
    if (controller < 0 || controller >= controller_total) return 0;
    device = &controllers[controller];
    if (!device->initialized || port < 0 || port >= device->port_count) return 0;
    if (device->pci.prog_if == USB_PROGIF_UHCI) {
        uint16_t io_base = (uint16_t)(device->pci.bar[4] & 0xFFFC);
        uint16_t port_register = (uint16_t)(io_base + uhci_port_offset(port));
        uint16_t status = inw(port_register);
        uint64_t start_ticks;

        if (!(status & UHCI_PORTSC_CCS)) return 0;
        outw(port_register, (status & UHCI_PORTSC_WRITABLE_STATE) |
                            UHCI_PORTSC_PR);
        start_ticks = g_ticks;
        for (uint32_t i = 0; i < 1000000; i++) {
            if (g_ticks - start_ticks >= TIMER_HZ / 20) break;
            __asm__ volatile("pause");
        }
        if (g_ticks - start_ticks < TIMER_HZ / 20) return 0;

        status = inw(port_register);
        outw(port_register, status & UHCI_PORTSC_WRITABLE_STATE);
        start_ticks = g_ticks;
        for (uint32_t i = 0; i < 1000000; i++) {
            status = inw(port_register);
            if (!(status & UHCI_PORTSC_CCS)) return 0;
            if (status & UHCI_PORTSC_PE) return 1;
            if (g_ticks - start_ticks >= TIMER_HZ / 10) break;
            __asm__ volatile("pause");
        }
    } else if (device->pci.prog_if == USB_PROGIF_EHCI) {
        offset = device->operational + 0x44 + port * 4;
        status = usb_mmio_read(device->mmio, offset);
        if (!(status & EHCI_PORTSC_CCS)) return 0;
        usb_mmio_write(device->mmio, offset,
                       (status & EHCI_PORTSC_PP) | EHCI_PORTSC_PR);
        for (uint32_t i = 0; i < 100000; i++) {
            status = usb_mmio_read(device->mmio, offset);
            if (!(status & EHCI_PORTSC_CCS)) return 0;
            if (!(status & EHCI_PORTSC_PR))
                return (status & EHCI_PORTSC_PE) != 0;
        }
    } else if (device->pci.prog_if == USB_PROGIF_XHCI) {
        uint32_t reset_bit;
        uint32_t reset_change_bit;
        offset = device->operational + 0x400 + port * 0x10;
        status = usb_mmio_read(device->mmio, offset);
        if (!(status & XHCI_PORTSC_CCS)) return 0;
        if (((status >> XHCI_PORTSC_SPEED_SHIFT) &
             XHCI_PORTSC_SPEED_MASK) >= 4) {
            reset_bit = XHCI_PORTSC_WPR;
            reset_change_bit = XHCI_PORTSC_WRC;
        } else {
            reset_bit = XHCI_PORTSC_PR;
            reset_change_bit = XHCI_PORTSC_PRC;
        }
        if (status & reset_change_bit) {
            usb_mmio_write(device->mmio, offset,
                           (status & XHCI_PORTSC_PP) |
                           reset_change_bit);
            status = usb_mmio_read(device->mmio, offset);
            if (!(status & XHCI_PORTSC_CCS)) return 0;
        }
        usb_mmio_write(device->mmio, offset,
                       (status & XHCI_PORTSC_PP) | reset_bit);
        for (uint32_t i = 0; i < 100000; i++) {
            status = usb_mmio_read(device->mmio, offset);
            if (!(status & XHCI_PORTSC_CCS)) return 0;
            if (!(status & reset_bit)) {
                if (status & reset_change_bit)
                    usb_mmio_write(device->mmio, offset,
                                   (status & XHCI_PORTSC_PP) |
                                   reset_change_bit);
                return (status & XHCI_PORTSC_PED) != 0;
            }
        }
    }
    return 0;
}

void usb_poll_hotplug(void)
{
    for (int controller = 0; controller < controller_total; controller++) {
        usb_controller_t* device = &controllers[controller];
        if (!device->initialized) continue;
        if (device->pci.prog_if == USB_PROGIF_XHCI) {
            xhci_trb_t event;
            while (xhci_next_event(device, &event))
                xhci_hid_handle_event(device, &event);
        }
        for (int port = 0; port < device->port_count; port++) {
            uint32_t offset;
            uint32_t status;
            uint32_t change_bits;
            int connected;
            if (device->pci.prog_if == USB_PROGIF_UHCI) {
                uint16_t io_base = (uint16_t)(device->pci.bar[4] & 0xFFFC);
                uint16_t port_register = (uint16_t)(
                    io_base + uhci_port_offset(port));
                uint16_t uhci_status = inw(port_register);
                change_bits = UHCI_PORTSC_CHANGE_BITS;
                status = uhci_status;
                connected = (status & UHCI_PORTSC_CCS) != 0;
                if (connected_state[controller][port] != connected) {
                    connected_state[controller][port] = (uint8_t)connected;
                    serial_puts("[usb] root port connection changed\n");
                }
                if (status & change_bits)
                    outw(port_register,
                         (uint16_t)((status & UHCI_PORTSC_WRITABLE_STATE) |
                                    (status & change_bits)));
                continue;
            } else if (device->pci.prog_if == USB_PROGIF_XHCI) {
                offset = device->operational + 0x400 + port * 0x10;
                change_bits = XHCI_PORTSC_CHANGE_BITS;
            } else if (device->pci.prog_if == USB_PROGIF_EHCI) {
                offset = device->operational + 0x44 + port * 4;
                change_bits = EHCI_PORTSC_CHANGE_BITS;
            } else {
                continue;
            }
            status = usb_mmio_read(device->mmio, offset);
            connected = (status & 1U) != 0;
            if (connected_state[controller][port] != connected) {
                int was_connected = connected_state[controller][port] != 0;
                connected_state[controller][port] = (uint8_t)connected;
                serial_puts("[usb] root port connection changed\n");
                if (device->pci.prog_if == USB_PROGIF_XHCI &&
                    device->slot_port == port && !connected) {
                    if (!usb_partition_remove_all(device))
                        serial_puts("[usb] ERROR: partition still registered after unplug\n");
                    if (device->storage_registered) {
                        vfs_forget_device(device->block_name);
                        if (block_unregister_device(device->block_name))
                            device->storage_registered = 0;
                        else
                            serial_puts("[usb] ERROR: storage still registered after unplug\n");
                    }
                    device->media_online = 0;
                    device->storage_ready = 0;
                    if (!xhci_release_device(device))
                        serial_puts("[usb] cannot release disconnected xHCI slot; DMA memory retained\n");
                    serial_puts("[usb] storage device disconnected\n");
                } else if (device->pci.prog_if == USB_PROGIF_XHCI &&
                           !was_connected && connected) {
                    uint32_t port_status = usb_mmio_read(
                        device->mmio,
                        device->operational + 0x400 + (uint32_t)port * 0x10);
                    enumeration_attempt_tick[controller][port] = g_ticks;
                    if (xhci_enumerate_mass_storage(device, port,
                                                    port_status) &&
                        device->storage_ready)
                        serial_puts("[usb] USB mass-storage device ready\n");
                }
            }
            if (device->pci.prog_if == USB_PROGIF_XHCI && connected &&
                !device->slot_id &&
                g_ticks - enumeration_attempt_tick[controller][port] >=
                    TIMER_HZ) {
                uint32_t port_status = usb_mmio_read(
                    device->mmio,
                    device->operational + 0x400 + (uint32_t)port * 0x10);
                enumeration_attempt_tick[controller][port] = g_ticks;
                if (xhci_enumerate_mass_storage(device, port, port_status) &&
                    device->storage_ready)
                    serial_puts("[usb] USB mass-storage device ready\n");
            }
            if (status & change_bits) {
                uint32_t writable_state = 0;
                if (device->pci.prog_if == USB_PROGIF_XHCI)
                    writable_state = status & XHCI_PORTSC_WRITABLE_STATE;
                else
                    writable_state = status & EHCI_PORTSC_WRITABLE_STATE;
                usb_mmio_write(device->mmio, offset,
                               writable_state | (status & change_bits));
            }
        }
    }
}
