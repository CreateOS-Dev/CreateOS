#ifndef CREATEOS_LWIP_PORT_H
#define CREATEOS_LWIP_PORT_H

#include <stddef.h>

struct netif;

int lwip_port_init(void);
void lwip_port_poll(void);
int lwip_port_receive(int interface_index, const void* frame, size_t length);
struct netif* lwip_port_netif(int interface_index);

#endif
