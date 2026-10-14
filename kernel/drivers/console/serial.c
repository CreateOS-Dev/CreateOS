#include "drivers/serial.h"
#include "drivers/framebuffer.h"
#include "arch/x86/io.h"
#include <stdint.h>

#define COM1_BASE 0x3F8
#define SERIAL_DATA (COM1_BASE + 0)
#define SERIAL_INTERRUPT_ENABLE (COM1_BASE + 1)
#define SERIAL_FIFO_CONTROL (COM1_BASE + 2)
#define SERIAL_LINE_CONTROL (COM1_BASE + 3)
#define SERIAL_MODEM_CONTROL (COM1_BASE + 4)
#define SERIAL_LINE_STATUS (COM1_BASE + 5)
#define SERIAL_TX_READY 0x20
#define SERIAL_RX_READY 0x01
#define SERIAL_RX_ERROR_MASK 0x9E
#define SERIAL_TX_POLL_LIMIT 100000U

static int serial_ready;
static int serial_faulted;
static int display_mirror;

static int serial_wait_transmit(void)
{
    for (uint32_t attempt = 0; attempt < SERIAL_TX_POLL_LIMIT; attempt++) {
        if (inb(SERIAL_LINE_STATUS) & SERIAL_TX_READY)
            return 1;
        __asm__ volatile("pause");
    }
    serial_faulted = 1;
    return 0;
}

static void serial_write_char(char c)
{
    if (!serial_ready || serial_faulted || !serial_wait_transmit())
        return;
    outb(SERIAL_DATA, (uint8_t)c);
}

int serial_init(void)
{
    serial_ready = 0;
    serial_faulted = 0;
    outb(SERIAL_INTERRUPT_ENABLE, 0x00);
    outb(SERIAL_LINE_CONTROL, 0x80);
    outb(SERIAL_DATA, 0x01);
    outb(SERIAL_INTERRUPT_ENABLE, 0x00);
    outb(SERIAL_LINE_CONTROL, 0x03);
    outb(SERIAL_FIFO_CONTROL, 0xC7);

    outb(SERIAL_MODEM_CONTROL, 0x1E);
    outb(SERIAL_DATA, 0xAE);
    if (inb(SERIAL_DATA) != 0xAE) {
        outb(SERIAL_MODEM_CONTROL, 0x00);
        return 0;
    }
    outb(SERIAL_MODEM_CONTROL, 0x0B);
    serial_ready = 1;
    return 1;
}

int serial_is_ready(void)
{
    return serial_ready && !serial_faulted;
}

void serial_putc(char c)
{
    serial_write_char(c);
}

void serial_puts_raw(const char* s)
{
    if (!s) return;
    while (*s) {
        char c = *s++;
        if (c == '\n')
            serial_write_char('\r');
        serial_write_char(c);
    }
}

void serial_puts(const char* s)
{
    if (!s) return;
    serial_puts_raw(s);
    if (display_mirror && framebuffer_ready())
        framebuffer_write(s, 0x07);
}

void serial_set_display_mirror(int enabled)
{
    display_mirror = enabled != 0;
}

char serial_getc(void)
{
    uint8_t status;
    if (!serial_is_ready())
        return 0;
    status = inb(SERIAL_LINE_STATUS);
    if (!(status & SERIAL_RX_READY))
        return 0;
    if (status & SERIAL_RX_ERROR_MASK) {
        (void)inb(SERIAL_DATA);
        return 0;
    }
    return (char)inb(SERIAL_DATA);
}
