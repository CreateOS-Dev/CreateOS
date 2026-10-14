#ifndef CREATEOS_LWIP_ARCH_CC_H
#define CREATEOS_LWIP_ARCH_CC_H

#include <stddef.h>
#include <stdint.h>
#include "lib/string.h"

#define BYTE_ORDER LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN 4321
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_CTYPE_H 1
#define LWIP_NO_UNISTD_H 1
#define X8_F "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "u"

typedef uintptr_t sys_prot_t;

uint32_t lwip_port_random(void);
void lwip_port_assert(const char* message, const char* file, int line);

#define LWIP_PLATFORM_DIAG(arguments) ((void)0)
#define LWIP_PLATFORM_ASSERT(message) \
    lwip_port_assert((message), __FILE__, __LINE__)

#endif
