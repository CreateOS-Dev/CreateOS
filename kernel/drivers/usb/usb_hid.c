// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "drivers/usb_hid.h"

#define HID_MOD_LEFT_CTRL  0x01
#define HID_MOD_LEFT_SHIFT 0x02
#define HID_MOD_RIGHT_SHIFT 0x20
#define HID_KEY_ERROR_ROLLOVER 0x01

static int hid_shifted(uint8_t modifiers)
{
    return (modifiers & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT)) != 0;
}

int usb_hid_parse_keyboard_report(const uint8_t* report,
                                  size_t report_length,
                                  usb_hid_keyboard_report_t* parsed)
{
    usb_hid_keyboard_report_t result;
    if (!report || !parsed || report_length != 8 || report[1] != 0)
        return 0;
    result.modifiers = report[0];
    result.reserved = report[1];
    for (int i = 0; i < 6; i++) {
        if (report[2 + i] == HID_KEY_ERROR_ROLLOVER ||
            (report[2 + i] > HID_KEY_ERROR_ROLLOVER &&
             report[2 + i] < 0x04))
            return 0;
        result.usages[i] = report[2 + i];
    }
    *parsed = result;
    return 1;
}

char usb_hid_usage_to_ascii(uint8_t usage, uint8_t modifiers)
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyz";
    static const char digits[] = "1234567890";
    static const char shifted_digits[] = "!@#$%^&*()";
    int shifted = hid_shifted(modifiers);

    if (usage >= 0x04 && usage <= 0x1D) {
        char value = letters[usage - 0x04];
        if (shifted) value = (char)(value - 'a' + 'A');
        return value;
    }
    if (usage >= 0x1E && usage <= 0x27)
        return shifted ? shifted_digits[usage - 0x1E] : digits[usage - 0x1E];
    if (usage == 0x28) return '\n';
    if (usage == 0x2C) return ' ';
    if (usage == 0x2D) return shifted ? '_' : '-';
    if (usage == 0x2E) return shifted ? '+' : '=';
    if (usage == 0x2F) return shifted ? '{' : '[';
    if (usage == 0x30) return shifted ? '}' : ']';
    if (usage == 0x31) return shifted ? '|' : '\\';
    if (usage == 0x33) return shifted ? ':' : ';';
    if (usage == 0x34) return shifted ? '"' : '\'';
    if (usage == 0x35) return shifted ? '~' : '`';
    if (usage == 0x36) return shifted ? '<' : ',';
    if (usage == 0x37) return shifted ? '>' : '.';
    if (usage == 0x38) return shifted ? '?' : '/';
    return 0;
}

int usb_hid_parse_mouse_report(const uint8_t* report,
                               size_t report_length,
                               usb_hid_mouse_report_t* parsed)
{
    usb_hid_mouse_report_t result;
    if (!report || !parsed || report_length < 3) return 0;
    result.buttons = report[0] & 0x07;
    result.x = (int8_t)report[1];
    result.y = (int8_t)report[2];
    result.wheel = report_length >= 4 ? (int8_t)report[3] : 0;
    *parsed = result;
    return 1;
}
