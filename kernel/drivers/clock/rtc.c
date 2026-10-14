// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "drivers/rtc.h"
#include "arch/x86_64/io.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71
#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS   0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B
#define RTC_UIP 0x80
#define RTC_BINARY_MODE 0x04

static uint8_t rtc_read_register(uint8_t reg)
{
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static uint8_t rtc_bcd_to_binary(uint8_t value)
{
    return (uint8_t)((value & 0x0F) + ((value >> 4) * 10));
}

static int rtc_wait_update(void)
{
    for (uint32_t i = 0; i < 100000; i++) {
        if (!(rtc_read_register(RTC_STATUS_A) & RTC_UIP)) return 1;
    }
    return 0;
}

void rtc_init(void)
{
    /* Keep the RTC periodic/update interrupt disabled until an IDT/IRQ8
     * handler exists; polling remains safe and deterministic for now. */
    uint8_t status = rtc_read_register(RTC_STATUS_B);
    outb(CMOS_INDEX, RTC_STATUS_B);
    outb(CMOS_DATA, (uint8_t)(status & (uint8_t)~0x40));
}

int rtc_read_time(rtc_time_t* out)
{
    rtc_time_t first;
    rtc_time_t second;
    uint8_t status;

    if (!out || !rtc_wait_update()) return 0;

    do {
        first.second = rtc_read_register(RTC_SECONDS);
        first.minute = rtc_read_register(RTC_MINUTES);
        first.hour = rtc_read_register(RTC_HOURS);
        first.day = rtc_read_register(RTC_DAY);
        first.month = rtc_read_register(RTC_MONTH);
        first.year = rtc_read_register(RTC_YEAR);
        status = rtc_read_register(RTC_STATUS_B);

        second.second = rtc_read_register(RTC_SECONDS);
        second.minute = rtc_read_register(RTC_MINUTES);
        second.hour = rtc_read_register(RTC_HOURS);
        second.day = rtc_read_register(RTC_DAY);
        second.month = rtc_read_register(RTC_MONTH);
        second.year = rtc_read_register(RTC_YEAR);
    } while (first.second != second.second || first.minute != second.minute ||
             first.hour != second.hour || first.day != second.day ||
             first.month != second.month || first.year != second.year);

    *out = second;
    if (!(status & RTC_BINARY_MODE)) {
        out->second = rtc_bcd_to_binary(out->second);
        out->minute = rtc_bcd_to_binary(out->minute);
        out->hour = rtc_bcd_to_binary(out->hour);
        out->day = rtc_bcd_to_binary(out->day);
        out->month = rtc_bcd_to_binary(out->month);
        out->year = rtc_bcd_to_binary(out->year);
    }
    return 1;
}
