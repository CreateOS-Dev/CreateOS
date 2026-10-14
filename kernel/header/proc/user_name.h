// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef KERNEL_USER_NAME_H
#define KERNEL_USER_NAME_H

#define USER_NAME_CAPACITY 32


static inline int user_name_valid(const char* name)
{
    int first = 1;
    if (!name || !name[0]) return 0;
    for (const char* p = name; *p; p++) {
        char c = *p;
        if (p - name >= USER_NAME_CAPACITY - 1) return 0;
        if (first) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9')))
                return 0;
            first = 0;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

static inline int user_name_creation_valid(const char* name)
{
    if (!name || !name[0]) return 0;
    for (const char* p = name; *p; p++) {
        char c = *p;
        if (p - name >= USER_NAME_CAPACITY - 1 ||
            !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            return 0;
    }
    return 1;
}

static inline int user_name_equal(const char* left, const char* right)
{
    if (!left || !right) return 0;
    while (*left && *right) {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return *left == 0 && *right == 0;
}

#endif
