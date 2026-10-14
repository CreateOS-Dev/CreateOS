// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef _STACK_PROTECTOR_H
#define _STACK_PROTECTOR_H

#include <stdint.h>

extern uintptr_t __stack_chk_guard;
void __stack_chk_fail(void);
int __stack_chk_init(void);

#endif