#include "drivers/netdev.h"
#include "lib/string.h"

typedef struct {
    netdev_info_t info;
    netdev_ops_t ops;
    void* context;
    int registered;
} netdev_entry_t;

static netdev_entry_t devices[NETDEV_MAX_DEVICES];
static int device_count;

int netdev_register(const char* name, const uint8_t mac[6],
                    size_t max_frame_size, const netdev_ops_t* ops,
                    void* context)
{
    size_t name_length = 0;
    int free_index = -1;

    if (!name || !mac || !ops || !context || !ops->send || !ops->receive ||
        !ops->link_up || max_frame_size < NETDEV_ETHERNET_HEADER_SIZE ||
        max_frame_size > NETDEV_MAX_FRAME_SIZE)
        return -1;
    while (name_length < NETDEV_NAME_MAX && name[name_length])
        name_length++;
    if (!name_length || name_length == NETDEV_NAME_MAX)
        return -1;

    for (int i = 0; i < NETDEV_MAX_DEVICES; i++) {
        if (!devices[i].registered) {
            if (free_index < 0) free_index = i;
            continue;
        }
        if (devices[i].context == context) {
            if (!strcmp(devices[i].info.name, name) &&
                devices[i].info.max_frame_size == max_frame_size &&
                memcmp(devices[i].info.mac, mac,
                       sizeof(devices[i].info.mac)) == 0 &&
                devices[i].ops.send == ops->send &&
                devices[i].ops.receive == ops->receive &&
                devices[i].ops.link_up == ops->link_up)
                return i;
            return -1;
        }
        if (!strcmp(devices[i].info.name, name))
            return -1;
        if (memcmp(devices[i].info.mac, mac,
                   sizeof(devices[i].info.mac)) == 0)
            return -1;
    }
    if (free_index < 0)
        return -1;

    netdev_entry_t* device = &devices[free_index];
    memset(device, 0, sizeof(*device));
    memcpy(device->info.name, name, name_length);
    memcpy(device->info.mac, mac, sizeof(device->info.mac));
    device->info.max_frame_size = (uint32_t)max_frame_size;
    device->ops = *ops;
    device->context = context;
    device->registered = 1;
    device_count++;
    return free_index;
}

int netdev_count(void)
{
    return device_count;
}

int netdev_get_info(int index, netdev_info_t* info)
{
    if (!info || index < 0 || index >= NETDEV_MAX_DEVICES ||
        !devices[index].registered)
        return 0;
    *info = devices[index].info;
    info->link_up =
        devices[index].ops.link_up(devices[index].context) != 0;
    return 1;
}

int netdev_link_up(int index)
{
    if (index < 0 || index >= NETDEV_MAX_DEVICES ||
        !devices[index].registered)
        return 0;
    return devices[index].ops.link_up(devices[index].context) != 0;
}

int netdev_send(int index, const void* frame, size_t length)
{
    netdev_entry_t* device;
    int result;

    if (index < 0 || index >= NETDEV_MAX_DEVICES ||
        !devices[index].registered || !frame ||
        length < NETDEV_ETHERNET_HEADER_SIZE ||
        length > devices[index].info.max_frame_size)
        return 0;
    device = &devices[index];
    result = device->ops.send(device->context, frame, length);
    return result == (int)length ? result : 0;
}

int netdev_receive(int index, void* frame, size_t capacity)
{
    netdev_entry_t* device;
    size_t bounded_capacity;
    int result;

    if (index < 0 || index >= NETDEV_MAX_DEVICES ||
        !devices[index].registered || !frame ||
        capacity < NETDEV_ETHERNET_HEADER_SIZE)
        return -1;
    device = &devices[index];
    bounded_capacity = capacity < device->info.max_frame_size
        ? capacity : device->info.max_frame_size;
    result = device->ops.receive(device->context, frame, bounded_capacity);
    if (result <= 0)
        return result;
    if ((size_t)result < NETDEV_ETHERNET_HEADER_SIZE ||
        (size_t)result > bounded_capacity)
        return -1;
    return result;
}

int netdev_set_ipv4(int index, const uint8_t address[4],
                    const uint8_t netmask[4], const uint8_t gateway[4],
                    const uint8_t dns[4], netdev_dhcp_state_t state)
{
    if (index < 0 || index >= NETDEV_MAX_DEVICES ||
        !devices[index].registered || state > NETDEV_DHCP_CONFLICT)
        return 0;
    if (address) memcpy(devices[index].info.ipv4_address, address, 4);
    else memset(devices[index].info.ipv4_address, 0, 4);
    if (netmask) memcpy(devices[index].info.ipv4_netmask, netmask, 4);
    else memset(devices[index].info.ipv4_netmask, 0, 4);
    if (gateway) memcpy(devices[index].info.ipv4_gateway, gateway, 4);
    else memset(devices[index].info.ipv4_gateway, 0, 4);
    if (dns) memcpy(devices[index].info.ipv4_dns, dns, 4);
    else memset(devices[index].info.ipv4_dns, 0, 4);
    devices[index].info.dhcp_state = (uint32_t)state;
    return 1;
}

int netdev_set_mac_is_local(int index, int is_local)
{
    if (index < 0 || index >= NETDEV_MAX_DEVICES ||
        !devices[index].registered)
        return 0;
    devices[index].info.mac_is_local = is_local != 0;
    return 1;
}

int netdev_mac_in_use(const uint8_t mac[6])
{
    if (!mac) return 1;
    for (int i = 0; i < NETDEV_MAX_DEVICES; i++) {
        if (devices[i].registered &&
            memcmp(devices[i].info.mac, mac,
                   sizeof(devices[i].info.mac)) == 0)
            return 1;
    }
    return 0;
}

int netdev_set_ipv6(int index, const uint8_t address[16],
                    uint32_t prefix_length, const uint8_t gateway[16],
                    const uint8_t dns[16], netdev_ipv6_state_t state)
{
    if (index < 0 || index >= NETDEV_MAX_DEVICES ||
        !devices[index].registered || state > NETDEV_IPV6_DUPLICATE ||
        prefix_length > 128)
        return 0;
    if (address) memcpy(devices[index].info.ipv6_address, address, 16);
    else memset(devices[index].info.ipv6_address, 0, 16);
    if (gateway) memcpy(devices[index].info.ipv6_gateway, gateway, 16);
    else memset(devices[index].info.ipv6_gateway, 0, 16);
    if (dns) memcpy(devices[index].info.ipv6_dns, dns, 16);
    else memset(devices[index].info.ipv6_dns, 0, 16);
    devices[index].info.ipv6_prefix_length = prefix_length;
    devices[index].info.ipv6_state = (uint32_t)state;
    return 1;
}
