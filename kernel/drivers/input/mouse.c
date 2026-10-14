// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "drivers/mouse.h"
#include <stdint.h>

#define KBD_STATUS 0x64
#define KBD_DATA 0x60
#define MOUSE_WRITE 0xD4
#define MOUSE_ENABLE 0xF4
#define MOUSE_SET_SAMPLE 0xF3
#define MOUSE_GET_ID 0xF2
#define MOUSE_ACK 0xFA

static uint8_t mouse_packet_size = 3;
#define USB_MOUSE_QUEUE_SIZE 32
static mouse_event_t usb_mouse_queue[USB_MOUSE_QUEUE_SIZE];
static uint8_t usb_mouse_head;
static uint8_t usb_mouse_tail;

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %%dx, %%al" : "=a"(value) : "d"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static int wait_write(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(KBD_STATUS) & 2)) return 1;
    return 0;
}

static int wait_read(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(KBD_STATUS) & 1) return 1;
    return 0;
}

static int mouse_command(uint8_t command, uint8_t* response) {
    if (!wait_write()) return 0;
    outb(KBD_STATUS, MOUSE_WRITE);
    if (!wait_write()) return 0;
    outb(KBD_DATA, command);
    if (!wait_read()) return 0;
    uint8_t ack = inb(KBD_DATA);
    if (response) *response = ack;
    return ack == MOUSE_ACK;
}

static int mouse_data(uint8_t value, uint8_t* response) {
    if (!wait_write()) return 0;
    outb(KBD_STATUS, MOUSE_WRITE);
    if (!wait_write()) return 0;
    outb(KBD_DATA, value);
    if (!wait_read()) return 0;
    uint8_t ack = inb(KBD_DATA);
    if (response) *response = ack;
    return ack == MOUSE_ACK;
}

static int mouse_query_id(uint8_t* id)
{
    uint8_t ack;
    if (!wait_write()) return 0;
    outb(KBD_STATUS, MOUSE_WRITE);
    if (!wait_write()) return 0;
    outb(KBD_DATA, MOUSE_GET_ID);
    if (!wait_read()) return 0;
    ack = inb(KBD_DATA);
    if (ack != MOUSE_ACK || !wait_read()) return 0;
    *id = inb(KBD_DATA);
    return 1;
}

int mouse_wheel_available(void)
{
    return mouse_packet_size == 4;
}

void mouse_init(void) {
    uint8_t response, id;
    mouse_packet_size = 3;

    if (!wait_write()) return;
    outb(KBD_STATUS, 0xA8);
    if (!mouse_command(MOUSE_SET_SAMPLE, &response) ||
        !mouse_data(200, &response) ||
        !mouse_command(MOUSE_SET_SAMPLE, &response) ||
        !mouse_data(100, &response) ||
        !mouse_command(MOUSE_SET_SAMPLE, &response) ||
        !mouse_data(80, &response))
        goto enable_mouse;

    if (mouse_query_id(&id) && (id == 0x03 || id == 0x04))
        mouse_packet_size = 4;

enable_mouse:
    (void)mouse_command(MOUSE_ENABLE, &response);
}

int mouse_poll_wheel(int* direction) {
    mouse_event_t event;

    if (!direction) return 0;
    *direction = 0;
    if (!mouse_poll_event(&event)) return 0;
    *direction = event.wheel;
    return event.wheel != 0;
}

int mouse_poll_event(mouse_event_t* event) {
    static uint8_t packet[4];
    static int index;
    uint8_t status;

    if (!event) return 0;
    event->dx = 0;
    event->dy = 0;
    event->wheel = 0;
    event->buttons = 0;
    if (usb_mouse_tail != usb_mouse_head) {
        *event = usb_mouse_queue[usb_mouse_tail];
        usb_mouse_tail =
            (uint8_t)((usb_mouse_tail + 1) % USB_MOUSE_QUEUE_SIZE);
        return 1;
    }
    status = inb(KBD_STATUS);
    if (!(status & 1) || !(status & 0x20)) return 0;

    if (index == 0) {
        uint8_t first = inb(KBD_DATA);
        if (!(first & 0x08)) return 0;
        packet[index++] = first;
    } else {
        packet[index++] = inb(KBD_DATA);
    }
    if (index < mouse_packet_size) return 0;
    index = 0;

    event->buttons = packet[0] & 0x07;
    if (!(packet[0] & 0x40)) event->dx = (int8_t)packet[1];
    if (!(packet[0] & 0x80)) event->dy = -(int8_t)packet[2];
    if (mouse_packet_size == 4) {
        if ((int8_t)packet[3] > 0) event->wheel = 1;
        if ((int8_t)packet[3] < 0) event->wheel = -1;
    }
    return 1;
}

void mouse_inject_event(const mouse_event_t* event)
{
    uint8_t next;
    if (!event) return;
    next = (uint8_t)((usb_mouse_head + 1) % USB_MOUSE_QUEUE_SIZE);
    if (next == usb_mouse_tail) return;
    usb_mouse_queue[usb_mouse_head] = *event;
    usb_mouse_head = next;
}
