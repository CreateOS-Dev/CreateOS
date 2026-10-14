#ifndef CREATEOS_E1000_H
#define CREATEOS_E1000_H

#include <stddef.h>
#include <stdint.h>
#include "drivers/netdev.h"

#define E1000_FRAME_MAX NETDEV_MAX_FRAME_SIZE

int e1000_init(void);
int e1000_available(void);
const uint8_t* e1000_mac_address(void);
int e1000_send(const void* frame, size_t length);
int e1000_receive(void* frame, size_t capacity);

#endif
