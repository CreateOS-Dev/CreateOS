#ifndef CREATEOS_NETDEV_H
#define CREATEOS_NETDEV_H

#include <stddef.h>
#include <stdint.h>

#define NETDEV_NAME_MAX 16
#define NETDEV_MAX_DEVICES 8
#define NETDEV_ETHERNET_HEADER_SIZE 14
#define NETDEV_MAX_FRAME_SIZE 1514

typedef enum {
    NETDEV_DHCP_IDLE = 0,
    NETDEV_DHCP_DISCOVERING,
    NETDEV_DHCP_REQUESTING,
    NETDEV_DHCP_PROBING,
    NETDEV_DHCP_BOUND,
    NETDEV_DHCP_RENEWING,
    NETDEV_DHCP_FAILED,
    NETDEV_DHCP_CONFLICT
} netdev_dhcp_state_t;

typedef enum {
    NETDEV_IPV6_DISABLED = 0,
    NETDEV_IPV6_TENTATIVE,
    NETDEV_IPV6_LINK_LOCAL,
    NETDEV_IPV6_CONFIGURED,
    NETDEV_IPV6_DUPLICATE
} netdev_ipv6_state_t;

typedef struct {
    int (*send)(void* context, const void* frame, size_t length);
    int (*receive)(void* context, void* frame, size_t capacity);
    int (*link_up)(void* context);
} netdev_ops_t;

typedef struct {
    char name[NETDEV_NAME_MAX];
    uint8_t mac[6];
    uint32_t max_frame_size;
    uint32_t link_up;
    uint32_t mac_is_local;
    uint32_t dhcp_state;
    uint8_t ipv4_address[4];
    uint8_t ipv4_netmask[4];
    uint8_t ipv4_gateway[4];
    uint8_t ipv4_dns[4];
    uint32_t ipv6_state;
    uint32_t ipv6_prefix_length;
    uint8_t ipv6_address[16];
    uint8_t ipv6_gateway[16];
    uint8_t ipv6_dns[16];
} netdev_info_t;

int netdev_register(const char* name, const uint8_t mac[6],
                    size_t max_frame_size, const netdev_ops_t* ops,
                    void* context);
int netdev_count(void);
int netdev_get_info(int index, netdev_info_t* info);
int netdev_link_up(int index);
int netdev_send(int index, const void* frame, size_t length);
int netdev_receive(int index, void* frame, size_t capacity);
int netdev_set_ipv4(int index, const uint8_t address[4],
                    const uint8_t netmask[4], const uint8_t gateway[4],
                    const uint8_t dns[4], netdev_dhcp_state_t state);
int netdev_set_mac_is_local(int index, int is_local);
int netdev_mac_in_use(const uint8_t mac[6]);
int netdev_set_ipv6(int index, const uint8_t address[16],
                    uint32_t prefix_length, const uint8_t gateway[16],
                    const uint8_t dns[16], netdev_ipv6_state_t state);

#endif
