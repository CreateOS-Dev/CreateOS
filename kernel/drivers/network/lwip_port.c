#include "drivers/lwip_port.h"

#include "arch/x86_64/random.h"
#include "drivers/netdev.h"
#include "drivers/serial.h"
#include "drivers/timer.h"
#include "lib/string.h"

#include "lwip/err.h"
#include "lwip/acd.h"
#include "lwip/ethip6.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/timeouts.h"
#include "lwip/ip6_addr.h"
#include "netif/ethernet.h"

static struct netif lwip_interfaces[NETDEV_MAX_DEVICES];
static uint8_t lwip_interface_ready[NETDEV_MAX_DEVICES];
static int lwip_initialized;
static uint32_t last_acd_tick;

uint32_t sys_now(void)
{
    return (uint32_t)((g_ticks * 1000U) / TIMER_HZ);
}

uint32_t lwip_port_random(void)
{
    uint32_t value = 0;
    if (!x86_64_random_bytes(&value, sizeof(value)))
        return (uint32_t)sys_now() ^ 0x9E3779B9U;
    return value;
}

void lwip_port_assert(const char* message, const char* file, int line)
{
    (void)file;
    (void)line;
    serial_puts("[lwip] assertion failed: ");
    serial_puts(message ? message : "(no message)");
    serial_puts("\n");
}

static err_t lwip_linkoutput(struct netif* interface, struct pbuf* packet)
{
    uint8_t frame[NETDEV_MAX_FRAME_SIZE];
    int index;
    if (!interface || !packet) return ERR_ARG;
    index = (int)(uintptr_t)interface->state;
    if (index < 0 || index >= NETDEV_MAX_DEVICES ||
        packet->tot_len > sizeof(frame) ||
        pbuf_copy_partial(packet, frame, packet->tot_len, 0) != packet->tot_len)
        return ERR_BUF;
    return netdev_send(index, frame, packet->tot_len) == packet->tot_len
               ? ERR_OK : ERR_IF;
}

static err_t lwip_netif_init(struct netif* interface)
{
    int index = (int)(uintptr_t)interface->state;
    netdev_info_t info;
    if (index < 0 || index >= NETDEV_MAX_DEVICES ||
        !netdev_get_info(index, &info))
        return ERR_IF;
    interface->name[0] = 'e';
    interface->name[1] = (char)('0' + index);
    interface->hwaddr_len = sizeof(info.mac);
    memcpy(interface->hwaddr, info.mac, sizeof(info.mac));
    interface->mtu = NETDEV_MAX_FRAME_SIZE - NETDEV_ETHERNET_HEADER_SIZE;
    interface->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                       NETIF_FLAG_ETHERNET;
#if LWIP_IPV6 && LWIP_IPV6_MLD
    interface->flags |= NETIF_FLAG_MLD6;
#endif
    interface->linkoutput = lwip_linkoutput;
    interface->output = etharp_output;
#if LWIP_IPV6
    interface->output_ip6 = ethip6_output;
#endif
    return ERR_OK;
}

static int lwip_add_interface(int index, const netdev_info_t* info)
{
    ip4_addr_t address;
    ip4_addr_t netmask;
    ip4_addr_t gateway;
    struct netif* interface = &lwip_interfaces[index];

    IP4_ADDR(&address, info->ipv4_address[0], info->ipv4_address[1],
             info->ipv4_address[2], info->ipv4_address[3]);
    IP4_ADDR(&netmask, info->ipv4_netmask[0], info->ipv4_netmask[1],
             info->ipv4_netmask[2], info->ipv4_netmask[3]);
    IP4_ADDR(&gateway, info->ipv4_gateway[0], info->ipv4_gateway[1],
             info->ipv4_gateway[2], info->ipv4_gateway[3]);

    if (!netif_add(interface, &address, &netmask, &gateway,
                   (void*)(uintptr_t)index, lwip_netif_init,
                   ethernet_input))
        return 0;
    lwip_interface_ready[index] = 1;
    netif_set_up(interface);
    if (info->link_up) netif_set_link_up(interface);
    else netif_set_link_down(interface);
#if LWIP_IPV6
    netif_create_ip6_linklocal_address(interface, 1);
    netif_set_ip6_autoconfig_enabled(interface, 1);
#endif
    if (index == 0) netif_set_default(interface);
    return 1;
}

int lwip_port_init(void)
{
    if (lwip_initialized) return 1;
    memset(lwip_interfaces, 0, sizeof(lwip_interfaces));
    memset(lwip_interface_ready, 0, sizeof(lwip_interface_ready));
    lwip_init();
    last_acd_tick = sys_now();
    lwip_initialized = 1;
    lwip_port_poll();
    return 1;
}

void lwip_port_poll(void)
{
    if (!lwip_initialized) return;
    for (int index = 0; index < NETDEV_MAX_DEVICES; index++) {
        netdev_info_t info;
        struct netif* interface = &lwip_interfaces[index];
        if (!netdev_get_info(index, &info)) continue;
        if (!lwip_interface_ready[index]) {
            if (!lwip_add_interface(index, &info))
                serial_puts("[lwip] could not attach registered network device\n");
            continue;
        }
        if (info.link_up) netif_set_link_up(interface);
        else netif_set_link_down(interface);
    }
    sys_check_timeouts();
#if LWIP_ACD
    uint32_t now = sys_now();
    uint32_t elapsed_ms = now - last_acd_tick;
    if (elapsed_ms >= ACD_TMR_INTERVAL) {
        uint32_t ticks = elapsed_ms / ACD_TMR_INTERVAL;
        if (ticks > 100) ticks = 100;
        last_acd_tick += ticks * ACD_TMR_INTERVAL;
        while (ticks--)
            acd_tmr();
    }
#endif
}

struct netif* lwip_port_netif(int interface_index)
{
    if (interface_index < 0 || interface_index >= NETDEV_MAX_DEVICES ||
        !lwip_interface_ready[interface_index])
        return NULL;
    return &lwip_interfaces[interface_index];
}

int lwip_port_receive(int interface_index, const void* frame, size_t length)
{
    struct netif* interface;
    struct pbuf* packet;
    err_t result;
    if (!lwip_initialized || interface_index < 0 ||
        interface_index >= NETDEV_MAX_DEVICES || !frame ||
        length < NETDEV_ETHERNET_HEADER_SIZE ||
        length > NETDEV_MAX_FRAME_SIZE ||
        !lwip_interface_ready[interface_index])
        return 0;
    interface = &lwip_interfaces[interface_index];
    packet = pbuf_alloc(PBUF_RAW, (u16_t)length, PBUF_POOL);
    if (!packet) return 0;
    if (pbuf_take(packet, frame, (u16_t)length) != ERR_OK) {
        pbuf_free(packet);
        return 0;
    }
    result = interface->input(packet, interface);
    if (result != ERR_OK) pbuf_free(packet);
    return result == ERR_OK;
}
