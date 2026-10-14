// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "security/user_access.h"
#include "lib/string.h"
#include "memory/vmm.h"

static int security_valid_range(const void* ptr, size_t size, int require_write)
{
    uint64_t start = (uint64_t)ptr;

    if (start < VMM_USER_ADDRESS_BASE ||
        start >= VMM_USER_ADDRESS_LIMIT) {
        return 0;
    }
    if (size > VMM_USER_ADDRESS_LIMIT - start) {
        return 0;
    }
    return vmm_user_range_accessible(start, size, require_write);
}

int security_valid_user_range(const void* ptr, size_t size)
{
    return security_valid_range(ptr, size, 0);
}

int security_valid_user_write_range(const void* ptr, size_t size)
{
    return security_valid_range(ptr, size, 1);
}

int security_valid_user_string(const char* ptr, size_t max_len)
{
    uint64_t cursor = (uint64_t)(uintptr_t)ptr;
    size_t remaining = max_len;

    if (!ptr || !max_len) return 0;
    while (remaining) {
        uint64_t page_remaining = 0x1000 - (cursor & 0xFFF);
        uint64_t address_remaining;
        size_t chunk;
        const char* bytes;

        if (cursor < VMM_USER_ADDRESS_BASE ||
            cursor >= VMM_USER_ADDRESS_LIMIT)
            return 0;
        address_remaining = VMM_USER_ADDRESS_LIMIT - cursor;
        chunk = remaining;
        if ((uint64_t)chunk > page_remaining)
            chunk = (size_t)page_remaining;
        if ((uint64_t)chunk > address_remaining)
            chunk = (size_t)address_remaining;
        if (!security_valid_user_range((const void*)(uintptr_t)cursor, chunk))
            return 0;

        bytes = (const char*)(uintptr_t)cursor;
        for (size_t i = 0; i < chunk; i++) {
            if (bytes[i] == '\0') return 1;
        }
        cursor += chunk;
        remaining -= chunk;
    }
    return 0;
}

int security_valid_user_argv(const char* const* argv, size_t max_args,
                             size_t max_string_len)
{
    uint64_t base = (uint64_t)(uintptr_t)argv;

    if (!argv) return 1;
    for (size_t i = 0; i < max_args; i++) {
        uint64_t offset;
        uint64_t slot;
        const char* value;
        if ((uint64_t)i > UINT64_MAX / sizeof(*argv)) return 0;
        offset = (uint64_t)i * sizeof(*argv);
        if (offset > UINT64_MAX - base) return 0;
        slot = base + offset;
        if (!security_copy_from_user(&value,
                                     (const void*)(uintptr_t)slot,
                                     sizeof(value)))
            return 0;
        if (!value) return 1;
        if (!security_valid_user_string(value, max_string_len)) return 0;
    }
    return 0;
}

int security_copy_from_user(void* dest, const void* src, size_t size) {
    if (!security_valid_user_range(src, size)) {
        return 0;
    }
    memcpy(dest, src, size);
    return 1;
}

int security_copy_to_user(void* dest, const void* src, size_t size) {
    if (!security_valid_user_write_range(dest, size)) {
        return 0;
    }
    memcpy(dest, src, size);
    return 1;
}

int security_copy_user_string(char* dest, size_t dest_size,
                              const char* src, size_t max_len)
{
    size_t length = 0;
    if (!dest || !dest_size || !src || !max_len ||
        !security_valid_user_string(src, max_len))
        return 0;
    while (length + 1 < max_len && src[length]) length++;
    if (length + 1 > dest_size) return 0;
    if (!security_copy_from_user(dest, src, length + 1)) return 0;
    dest[length] = 0;
    return 1;
}