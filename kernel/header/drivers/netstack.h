#ifndef CREATEOS_NETSTACK_H
#define CREATEOS_NETSTACK_H

#include <stdint.h>

#define NET_TCP_MAX_REQUEST 2048
#define NET_TCP_MAX_RESPONSE 4096
#define NET_TCP_STREAM_IO_MAX 4096
#define NET_DNS_MAX_NAME_LENGTH 253

int net_dhcp_start(int interface_index);
int net_ping_ipv4(int interface_index, const uint8_t address[4],
                  uint32_t timeout_ms, uint32_t* round_trip_ms);
int net_resolve_ipv4(int interface_index, const char* hostname,
                     uint32_t timeout_ms, uint8_t address[4]);
int net_tcp_exchange_ipv4(int interface_index, const uint8_t address[4],
                          uint16_t port, const uint8_t* request,
                          uint32_t request_length, uint8_t* response,
                          uint32_t response_capacity, uint32_t timeout_ms,
                          uint32_t* response_length);
int net_tcp_stream_open_ipv4(int interface_index, const uint8_t address[4],
                             uint16_t port, uint32_t timeout_ms);
int net_tcp_stream_write(int handle, const uint8_t* data, uint32_t length,
                         uint32_t timeout_ms);
int net_tcp_stream_read(int handle, uint8_t* data, uint32_t capacity,
                        uint32_t timeout_ms);
int net_tcp_stream_close(int handle);
int net_tcp_listener_open_ipv4(int interface_index, uint16_t port);
int net_tcp_listener_accept(int handle, uint32_t timeout_ms);
int net_tcp_listener_close(int handle);
void net_stack_init(void);
void net_stack_poll(void);

#endif
