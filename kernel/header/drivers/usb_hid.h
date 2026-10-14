// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef USB_HID_H
#define USB_HID_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t modifiers;
    uint8_t reserved;
    uint8_t usages[6];
} usb_hid_keyboard_report_t;

typedef struct {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;
} usb_hid_mouse_report_t;

int usb_hid_parse_keyboard_report(const uint8_t* report,
                                  size_t report_length,
                                  usb_hid_keyboard_report_t* parsed);
char usb_hid_usage_to_ascii(uint8_t usage, uint8_t modifiers);
int usb_hid_parse_mouse_report(const uint8_t* report,
                               size_t report_length,
                               usb_hid_mouse_report_t* parsed);

#endif
