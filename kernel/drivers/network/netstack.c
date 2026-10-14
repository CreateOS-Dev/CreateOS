#include "drivers/netstack.h"

#include "arch/x86_64/random.h"
#include "drivers/lwip_port.h"
#include "drivers/netdev.h"
#include "drivers/timer.h"
#include "drivers/tty.h"
#include "lib/string.h"

#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/ip.h"
#include "lwip/raw.h"
#include "lwip/sys.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"

#define NET_POLL_FRAMES 8
#define NET_TCP_STREAM_BUFFER_SIZE 16384

typedef struct {
    struct raw_pcb* pcb;
    ip_addr_t target;
    uint32_t started;
    uint32_t elapsed;
    uint16_t identifier;
    uint16_t sequence;
    int active;
    int replied;
} ping_request_t;

typedef struct {
    int active;
    int complete;
    int success;
    ip_addr_t address;
} dns_request_t;

typedef struct {
    struct tcp_pcb* pcb;
    const uint8_t* request;
    uint16_t request_length;
    uint8_t* response;
    uint32_t response_capacity;
    uint32_t response_length;
    uint32_t started;
    int active;
    int complete;
    int failed;
} tcp_request_t;

typedef struct {
    struct tcp_pcb* pcb;
    uint8_t receive_buffer[NET_TCP_STREAM_BUFFER_SIZE];
    uint32_t receive_head;
    uint32_t receive_tail;
    uint32_t started;
    uint32_t handle;
    int active;
    int connected;
    int peer_closed;
    int failed;
} tcp_stream_t;

typedef struct {
    struct tcp_pcb* pcb;
    uint32_t handle;
    uint32_t accepted_handle;
    int active;
    int accepted;
} tcp_listener_t;

static ping_request_t ping_requests[NETDEV_MAX_DEVICES];
static dns_request_t dns_request;
static tcp_request_t tcp_request;
static tcp_stream_t tcp_stream;
static tcp_listener_t tcp_listener;
static uint16_t next_ping_identifier = 1;
static uint16_t next_tcp_port = 49152;
static int stack_initialized;

static int net_cancel_requested(void)
{
    return tty_take_ctrl_c_interrupt();
}

static void copy_ip4_bytes(uint8_t destination[4], const ip4_addr_t* source)
{
    for (unsigned int i = 0; i < 4; i++)
        destination[i] = ip4_addr_get_byte(source, i);
}

static int elapsed(uint32_t started, uint32_t timeout_ms)
{
    return (uint32_t)(sys_now() - started) >= timeout_ms;
}

static struct netif* get_interface(int index)
{
    if (index < 0 || index >= NETDEV_MAX_DEVICES)
        return NULL;
    return lwip_port_netif(index);
}

static u8_t ping_receive(void* argument, struct raw_pcb* pcb,
                         struct pbuf* packet, const ip_addr_t* source)
{
    ping_request_t* request = argument;
    struct ip_hdr ip_header;
    struct icmp_echo_hdr header;
    u16_t ip_header_length;
    (void)pcb;
    if (!request || !request->active || !packet ||
        packet->tot_len < IP_HLEN + sizeof(header) ||
        pbuf_copy_partial(packet, &ip_header, sizeof(ip_header), 0) !=
            sizeof(ip_header))
        return 0;
    ip_header_length = IPH_HL_BYTES(&ip_header);
    if (IPH_V(&ip_header) != 4 || ip_header_length < IP_HLEN ||
        packet->tot_len < ip_header_length + sizeof(header) ||
        pbuf_copy_partial(packet, &header, sizeof(header),
                          ip_header_length) != sizeof(header) ||
        header.type != ICMP_ER || header.code != 0 ||
        lwip_ntohs(header.id) != request->identifier ||
        lwip_ntohs(header.seqno) != request->sequence ||
        !ip_addr_cmp(source, &request->target))
        return 0;
    request->elapsed = sys_now() - request->started;
    request->replied = 1;
    request->active = 0;
    return 0;
}

static void dns_found(const char* name, const ip_addr_t* address,
                      void* argument)
{
    dns_request_t* request = argument;
    (void)name;
    if (!request || !request->active)
        return;
    if (address && IP_IS_V4(address)) {
        ip_addr_copy(request->address, *address);
        request->success = 1;
    }
    request->complete = 1;
}

static void tcp_error(void* argument, err_t error)
{
    tcp_request_t* request = argument;
    (void)error;
    if (!request || !request->active)
        return;
    request->pcb = NULL;
    request->failed = 1;
    request->complete = 1;
}

static err_t tcp_connected(void* argument, struct tcp_pcb* pcb, err_t error)
{
    tcp_request_t* request = argument;
    err_t result;
    if (!request || !request->active)
        return ERR_ABRT;
    if (error != ERR_OK) {
        request->failed = 1;
        request->complete = 1;
        return ERR_OK;
    }
    if (request->request_length != 0) {
        result = tcp_write(pcb, request->request, request->request_length,
                           TCP_WRITE_FLAG_COPY);
        if (result != ERR_OK) {
            request->failed = 1;
            request->complete = 1;
            return ERR_OK;
        }
        result = tcp_output(pcb);
        if (result != ERR_OK) {
            request->failed = 1;
            request->complete = 1;
            return ERR_OK;
        }
    } else {
        result = tcp_shutdown(pcb, 0, 1);
        if (result != ERR_OK) {
            request->failed = 1;
            request->complete = 1;
            return ERR_OK;
        }
    }
    if (request->response_capacity == 0)
        request->complete = 1;
    return ERR_OK;
}

static err_t tcp_receive(void* argument, struct tcp_pcb* pcb,
                         struct pbuf* packet, err_t error)
{
    tcp_request_t* request = argument;
    uint32_t remaining;
    u16_t copied;
    if (!request || !request->active) {
        if (packet)
            pbuf_free(packet);
        return ERR_OK;
    }
    if (error != ERR_OK) {
        if (packet)
            pbuf_free(packet);
        request->failed = 1;
        request->complete = 1;
        return ERR_OK;
    }
    if (!packet) {
        request->complete = 1;
        return ERR_OK;
    }

    remaining = request->response_capacity - request->response_length;
    copied = (u16_t)(packet->tot_len < remaining ? packet->tot_len : remaining);
    if (copied != 0) {
        if (pbuf_copy_partial(packet,
                              request->response + request->response_length,
                              copied, 0) != copied) {
            request->failed = 1;
            request->complete = 1;
        } else {
            request->response_length += copied;
        }
    }
    tcp_recved(pcb, packet->tot_len);
    pbuf_free(packet);
    if (request->response_length == request->response_capacity)
        request->complete = 1;
    return ERR_OK;
}

static void tcp_stream_error(void* argument, err_t error)
{
    tcp_stream_t* stream = argument;
    (void)error;
    if (!stream || !stream->active)
        return;
    stream->pcb = NULL;
    stream->failed = 1;
}

static err_t tcp_stream_connected(void* argument, struct tcp_pcb* pcb,
                                  err_t error)
{
    tcp_stream_t* stream = argument;
    if (!stream || !stream->active)
        return ERR_ABRT;
    if (error != ERR_OK) {
        stream->failed = 1;
        return ERR_OK;
    }
    stream->pcb = pcb;
    stream->connected = 1;
    return ERR_OK;
}

static err_t tcp_stream_receive(void* argument, struct tcp_pcb* pcb,
                                struct pbuf* packet, err_t error)
{
    tcp_stream_t* stream = argument;
    uint32_t available;
    uint32_t free_space;
    uint16_t copied;
    if (!stream || !stream->active) {
        if (packet)
            pbuf_free(packet);
        return ERR_OK;
    }
    if (error != ERR_OK) {
        if (packet)
            pbuf_free(packet);
        stream->failed = 1;
        return ERR_OK;
    }
    if (!packet) {
        stream->peer_closed = 1;
        return ERR_OK;
    }

    available = stream->receive_tail - stream->receive_head;
    free_space = NET_TCP_STREAM_BUFFER_SIZE - available;
    if (packet->tot_len > free_space) {
        tcp_recved(pcb, packet->tot_len);
        pbuf_free(packet);
        stream->failed = 1;
        return ERR_OK;
    }
    {
        uint32_t offset = stream->receive_tail %
                          NET_TCP_STREAM_BUFFER_SIZE;
        uint32_t first = packet->tot_len <
            NET_TCP_STREAM_BUFFER_SIZE - offset
            ? packet->tot_len : NET_TCP_STREAM_BUFFER_SIZE - offset;
        copied = pbuf_copy_partial(packet, stream->receive_buffer + offset,
                                   (u16_t)first, 0);
        if (copied == first && packet->tot_len > first) {
            uint16_t second = pbuf_copy_partial(
                packet, stream->receive_buffer,
                (u16_t)(packet->tot_len - first), (u16_t)first);
            copied = (uint16_t)(copied + second);
        }
    }
    if (copied != packet->tot_len) {
        tcp_recved(pcb, packet->tot_len);
        pbuf_free(packet);
        stream->failed = 1;
        return ERR_OK;
    }
    stream->receive_tail += copied;
    tcp_recved(pcb, packet->tot_len);
    pbuf_free(packet);
    return ERR_OK;
}

static err_t tcp_listener_accept(void* argument, struct tcp_pcb* pcb,
                                 err_t error)
{
    tcp_listener_t* listener = argument;
    tcp_stream_t* stream = &tcp_stream;
    uint32_t handle;
    if (!listener || !listener->active || !pcb || error != ERR_OK) {
        if (pcb)
            tcp_abort(pcb);
        return ERR_ABRT;
    }
    if (stream->active ||
        !x86_64_random_bytes(&handle, sizeof(handle))) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    handle &= INT32_MAX;
    if (!handle) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    memset(stream, 0, sizeof(*stream));
    stream->handle = handle;
    stream->active = 1;
    stream->connected = 1;
    stream->pcb = pcb;
    listener->accepted_handle = handle;
    listener->accepted = 1;
    tcp_arg(pcb, stream);
    tcp_recv(pcb, tcp_stream_receive);
    tcp_err(pcb, tcp_stream_error);
    return ERR_OK;
}

void net_stack_init(void)
{
    if (stack_initialized)
        return;
    memset(ping_requests, 0, sizeof(ping_requests));
    memset(&dns_request, 0, sizeof(dns_request));
    memset(&tcp_request, 0, sizeof(tcp_request));
    memset(&tcp_stream, 0, sizeof(tcp_stream));
    memset(&tcp_listener, 0, sizeof(tcp_listener));
    stack_initialized = lwip_port_init();
}

int net_dhcp_start(int interface_index)
{
    struct netif* interface;
    if (!stack_initialized)
        net_stack_init();
    lwip_port_poll();
    interface = get_interface(interface_index);
    if (!interface || !netif_is_up(interface) ||
        dhcp_start(interface) != ERR_OK)
        return 0;
    (void)netdev_set_ipv4(interface_index, NULL, NULL, NULL, NULL,
                          NETDEV_DHCP_DISCOVERING);
    return 1;
}

int net_ping_ipv4(int interface_index, const uint8_t address[4],
                  uint32_t timeout_ms, uint32_t* round_trip_ms)
{
    ping_request_t* request;
    struct netif* interface;
    struct pbuf* packet;
    struct icmp_echo_hdr* header;
    ip_addr_t destination;
    err_t result;
    uint32_t started;
    int interrupted = 0;
    if (!address || !round_trip_ms || !stack_initialized)
        return 0;
    interface = get_interface(interface_index);
    if (!interface)
        return 0;
    request = &ping_requests[interface_index];
    if (request->active)
        return 0;

    IP_ADDR4(&destination, address[0], address[1], address[2], address[3]);
    request->pcb = raw_new(IP_PROTO_ICMP);
    if (!request->pcb)
        return 0;
    request->identifier = next_ping_identifier++;
    request->sequence++;
    request->target = destination;
    request->started = sys_now();
    request->active = 1;
    request->replied = 0;
    raw_bind_netif(request->pcb, interface);
    raw_recv(request->pcb, ping_receive, request);

    packet = pbuf_alloc(PBUF_IP, sizeof(*header) + 16, PBUF_RAM);
    if (!packet) {
        raw_remove(request->pcb);
        request->pcb = NULL;
        request->active = 0;
        return 0;
    }
    header = packet->payload;
    memset(header, 0, packet->tot_len);
    ICMPH_TYPE_SET(header, ICMP_ECHO);
    ICMPH_CODE_SET(header, 0);
    header->id = lwip_htons(request->identifier);
    header->seqno = lwip_htons(request->sequence);
    header->chksum = inet_chksum(header, packet->tot_len);
    result = raw_sendto(request->pcb, packet, &destination);
    pbuf_free(packet);
    if (result != ERR_OK) {
        raw_remove(request->pcb);
        request->pcb = NULL;
        request->active = 0;
        return 0;
    }

    if (timeout_ms == 0)
        timeout_ms = 1;
    started = sys_now();
    while (!request->replied && !elapsed(started, timeout_ms)) {
        if (net_cancel_requested()) {
            interrupted = 1;
            break;
        }
        net_stack_poll();
        __asm__ volatile("pause");
    }
    if (request->pcb) {
        raw_remove(request->pcb);
        request->pcb = NULL;
    }
    request->active = 0;
    if (interrupted)
        return -2;
    if (!request->replied)
        return 0;
    *round_trip_ms = request->elapsed;
    return 1;
}

int net_resolve_ipv4(int interface_index, const char* hostname,
                     uint32_t timeout_ms, uint8_t address[4])
{
    struct netif* interface;
    ip_addr_t resolved;
    err_t result;
    uint32_t started;
    int interrupted = 0;
    size_t length = 0;
    if (!hostname || !address || !stack_initialized || dns_request.active)
        return 0;
    while (hostname[length] && length <= NET_DNS_MAX_NAME_LENGTH)
        length++;
    if (length == 0 || length > NET_DNS_MAX_NAME_LENGTH)
        return 0;
    interface = get_interface(interface_index);
    if (!interface)
        return 0;

    dns_request.active = 1;
    dns_request.complete = 0;
    dns_request.success = 0;
    netif_set_default(interface);
    result = dns_gethostbyname(hostname, &resolved, dns_found, &dns_request);
    if (result == ERR_OK) {
        dns_request.address = resolved;
        dns_request.success = IP_IS_V4(&resolved);
        dns_request.complete = 1;
    } else if (result != ERR_INPROGRESS) {
        dns_request.active = 0;
        return 0;
    }
    if (timeout_ms == 0)
        timeout_ms = 1;
    started = sys_now();
    while (!dns_request.complete && !elapsed(started, timeout_ms)) {
        if (net_cancel_requested()) {
            interrupted = 1;
            break;
        }
        net_stack_poll();
        __asm__ volatile("pause");
    }
    if (dns_request.success)
        copy_ip4_bytes(address, ip_2_ip4(&dns_request.address));
    dns_request.active = 0;
    if (interrupted)
        return -2;
    return dns_request.success;
}

int net_tcp_exchange_ipv4(int interface_index, const uint8_t address[4],
                          uint16_t port, const uint8_t* request,
                          uint32_t request_length, uint8_t* response,
                          uint32_t response_capacity, uint32_t timeout_ms,
                          uint32_t* response_length)
{
    struct netif* interface;
    struct tcp_pcb* pcb;
    ip_addr_t destination;
    err_t result;
    uint32_t started;
    int interrupted = 0;
    tcp_request_t* state = &tcp_request;
    if (!stack_initialized || !address || port == 0 ||
        (!request && request_length != 0) ||
        request_length > NET_TCP_MAX_REQUEST ||
        (!response && response_capacity != 0) ||
        response_capacity > NET_TCP_MAX_RESPONSE || !response_length ||
        state->active || tcp_stream.active)
        return -1;
    *response_length = 0;
    interface = get_interface(interface_index);
    if (!interface)
        return -1;

    memset(state, 0, sizeof(*state));
    state->active = 1;
    state->request = request;
    state->request_length = (uint16_t)request_length;
    state->response = response;
    state->response_capacity = response_capacity;
    state->started = sys_now();
    state->pcb = pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        state->active = 0;
        return -1;
    }
    tcp_arg(pcb, state);
    tcp_recv(pcb, tcp_receive);
    tcp_err(pcb, tcp_error);
    tcp_bind_netif(pcb, interface);
    result = tcp_bind(pcb, IP4_ADDR_ANY, next_tcp_port++);
    if (result != ERR_OK) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_abort(pcb);
        state->pcb = NULL;
        state->active = 0;
        return -1;
    }
    IP_ADDR4(&destination, address[0], address[1], address[2], address[3]);
    result = tcp_connect(pcb, &destination, port, tcp_connected);
    if (result != ERR_OK) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_abort(pcb);
        state->pcb = NULL;
        state->active = 0;
        return -1;
    }

    if (timeout_ms == 0)
        timeout_ms = 1;
    started = sys_now();
    while (!state->complete && !elapsed(started, timeout_ms)) {
        if (net_cancel_requested()) {
            interrupted = 1;
            break;
        }
        net_stack_poll();
        __asm__ volatile("pause");
    }
    state->active = 0;
    if (state->pcb) {
        tcp_arg(state->pcb, NULL);
        tcp_recv(state->pcb, NULL);
        tcp_err(state->pcb, NULL);
        if (tcp_close(state->pcb) != ERR_OK)
            tcp_abort(state->pcb);
        state->pcb = NULL;
    }
    *response_length = state->response_length;
    if (interrupted)
        return -2;
    return state->complete && !state->failed ? 0 : -1;
}

int net_tcp_stream_open_ipv4(int interface_index, const uint8_t address[4],
                             uint16_t port, uint32_t timeout_ms)
{
    struct netif* interface;
    struct tcp_pcb* pcb;
    ip_addr_t destination;
    err_t result;
    uint32_t started;
    int interrupted = 0;
    tcp_stream_t* stream = &tcp_stream;
    if (!stack_initialized || !address || !port || stream->active ||
        tcp_request.active)
        return -1;
    interface = get_interface(interface_index);
    if (!interface)
        return -1;

    memset(stream, 0, sizeof(*stream));
    stream->active = 1;
    stream->started = sys_now();
    if (!x86_64_random_bytes(&stream->handle, sizeof(stream->handle))) {
        memset(stream, 0, sizeof(*stream));
        return -1;
    }
    stream->handle &= INT32_MAX;
    if (stream->handle == 0) {
        memset(stream, 0, sizeof(*stream));
        return -1;
    }
    stream->pcb = pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        memset(stream, 0, sizeof(*stream));
        return -1;
    }
    tcp_arg(pcb, stream);
    tcp_recv(pcb, tcp_stream_receive);
    tcp_err(pcb, tcp_stream_error);
    tcp_bind_netif(pcb, interface);
    result = tcp_bind(pcb, IP4_ADDR_ANY, next_tcp_port++);
    if (result != ERR_OK) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_abort(pcb);
        memset(stream, 0, sizeof(*stream));
        return -1;
    }
    IP_ADDR4(&destination, address[0], address[1], address[2], address[3]);
    result = tcp_connect(pcb, &destination, port, tcp_stream_connected);
    if (result != ERR_OK) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_abort(pcb);
        memset(stream, 0, sizeof(*stream));
        return -1;
    }

    if (timeout_ms == 0)
        timeout_ms = 1;
    started = sys_now();
    while (!stream->connected && !stream->failed &&
           !elapsed(started, timeout_ms)) {
        if (net_cancel_requested()) {
            interrupted = 1;
            break;
        }
        net_stack_poll();
        __asm__ volatile("pause");
    }
    if (stream->connected)
        return (int)stream->handle;

    stream->active = 0;
    if (stream->pcb) {
        tcp_arg(stream->pcb, NULL);
        tcp_recv(stream->pcb, NULL);
        tcp_err(stream->pcb, NULL);
        if (tcp_close(stream->pcb) != ERR_OK)
            tcp_abort(stream->pcb);
    }
    memset(stream, 0, sizeof(*stream));
    return interrupted ? -2 : -1;
}

int net_tcp_stream_write(int handle, const uint8_t* data, uint32_t length,
                         uint32_t timeout_ms)
{
    tcp_stream_t* stream = &tcp_stream;
    uint32_t written = 0;
    uint32_t started;
    if (!stream->active || !stream->connected || stream->failed ||
        (uint32_t)handle != stream->handle || (!data && length) ||
        length > NET_TCP_STREAM_IO_MAX)
        return -1;
    if (timeout_ms == 0)
        timeout_ms = 1;
    started = sys_now();
    while (written < length && stream->pcb && !stream->failed) {
        if (net_cancel_requested())
            return -2;
        u16_t available = tcp_sndbuf(stream->pcb);
        uint32_t remaining = length - written;
        u16_t chunk = (u16_t)(remaining < available ? remaining : available);
        err_t result;
        if (chunk != 0) {
            result = tcp_write(stream->pcb, data + written, chunk,
                               TCP_WRITE_FLAG_COPY);
            if (result == ERR_OK) {
                result = tcp_output(stream->pcb);
                if (result != ERR_OK)
                    break;
                written += chunk;
                continue;
            }
            if (result != ERR_MEM)
                break;
        }
        if (elapsed(started, timeout_ms))
            break;
        net_stack_poll();
        __asm__ volatile("pause");
    }
    return written ? (int)written : (length == 0 ? 0 : -1);
}

int net_tcp_stream_read(int handle, uint8_t* data, uint32_t capacity,
                        uint32_t timeout_ms)
{
    tcp_stream_t* stream = &tcp_stream;
    uint32_t available;
    uint32_t copied;
    uint32_t started;
    int interrupted = 0;
    if (!stream->active || !stream->connected ||
        (uint32_t)handle != stream->handle || !data ||
        capacity == 0 || capacity > NET_TCP_STREAM_IO_MAX)
        return -1;
    if (timeout_ms == 0)
        timeout_ms = 1;
    started = sys_now();
    for (;;) {
        available = stream->receive_tail - stream->receive_head;
        if (available != 0 || stream->peer_closed || stream->failed)
            break;
        if (net_cancel_requested()) {
            interrupted = 1;
            break;
        }
        if (elapsed(started, timeout_ms))
            return -1;
        net_stack_poll();
        __asm__ volatile("pause");
    }
    if (interrupted)
        return -2;
    if (available == 0)
        return stream->failed ? -1 : 0;
    copied = available < capacity ? available : capacity;
    {
        uint32_t offset = stream->receive_head %
                          NET_TCP_STREAM_BUFFER_SIZE;
        uint32_t first = copied <
            NET_TCP_STREAM_BUFFER_SIZE - offset
            ? copied : NET_TCP_STREAM_BUFFER_SIZE - offset;
        memcpy(data, stream->receive_buffer + offset, first);
        if (copied > first)
            memcpy(data + first, stream->receive_buffer, copied - first);
    }
    stream->receive_head += copied;
    return (int)copied;
}

int net_tcp_stream_close(int handle)
{
    tcp_stream_t* stream = &tcp_stream;
    if (!stream->active || (uint32_t)handle != stream->handle)
        return -1;
    stream->active = 0;
    if (stream->pcb) {
        tcp_arg(stream->pcb, NULL);
        tcp_recv(stream->pcb, NULL);
        tcp_err(stream->pcb, NULL);
        if (tcp_close(stream->pcb) != ERR_OK)
            tcp_abort(stream->pcb);
    }
    memset(stream, 0, sizeof(*stream));
    if (tcp_listener.active) {
        tcp_listener.accepted = 0;
        tcp_listener.accepted_handle = 0;
    }
    return 0;
}

int net_tcp_listener_open_ipv4(int interface_index, uint16_t port)
{
    struct netif* interface;
    struct tcp_pcb* pcb;
    struct tcp_pcb* listening_pcb;
    tcp_listener_t* listener = &tcp_listener;
    if (!stack_initialized || !port || listener->active ||
        tcp_stream.active || tcp_request.active)
        return -1;
    interface = get_interface(interface_index);
    if (!interface)
        return -1;

    memset(listener, 0, sizeof(*listener));
    if (!x86_64_random_bytes(&listener->handle, sizeof(listener->handle)))
        return -1;
    listener->handle &= INT32_MAX;
    if (!listener->handle) {
        memset(listener, 0, sizeof(*listener));
        return -1;
    }

    pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb)
        return -1;
    tcp_bind_netif(pcb, interface);
    if (tcp_bind(pcb, IP4_ADDR_ANY, port) != ERR_OK) {
        tcp_abort(pcb);
        memset(listener, 0, sizeof(*listener));
        return -1;
    }
    listening_pcb = tcp_listen_with_backlog(pcb, 1);
    if (!listening_pcb) {
        tcp_abort(pcb);
        memset(listener, 0, sizeof(*listener));
        return -1;
    }

    listener->pcb = listening_pcb;
    listener->active = 1;
    tcp_arg(listening_pcb, listener);
    tcp_accept(listening_pcb, tcp_listener_accept);
    return (int)listener->handle;
}

int net_tcp_listener_accept(int handle, uint32_t timeout_ms)
{
    tcp_listener_t* listener = &tcp_listener;
    uint32_t started;
    if (!listener->active || listener->handle != (uint32_t)handle)
        return -1;
    if (timeout_ms == 0)
        timeout_ms = 1;
    started = sys_now();
    while (listener->active && !listener->accepted &&
           !elapsed(started, timeout_ms)) {
        if (net_cancel_requested())
            return -2;
        net_stack_poll();
        __asm__ volatile("pause");
    }
    return listener->accepted ? (int)listener->accepted_handle : -1;
}

int net_tcp_listener_close(int handle)
{
    tcp_listener_t* listener = &tcp_listener;
    if (!listener->active || listener->handle != (uint32_t)handle)
        return -1;
    listener->active = 0;
    if (listener->accepted && tcp_stream.active &&
        net_tcp_stream_close((int)listener->accepted_handle) != 0) {
        listener->active = 1;
        return -1;
    }
    if (listener->pcb) {
        tcp_arg(listener->pcb, NULL);
        tcp_accept(listener->pcb, NULL);
        if (tcp_close(listener->pcb) != ERR_OK)
            tcp_abort(listener->pcb);
    }
    memset(listener, 0, sizeof(*listener));
    return 0;
}

static void update_device_configuration(int index, struct netif* interface)
{
    netdev_info_t info;
    uint8_t address[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t dns[4] = { 0 };
    const ip_addr_t* dns_server;
    netdev_dhcp_state_t state = NETDEV_DHCP_DISCOVERING;
    if (!netdev_get_info(index, &info))
        return;
    if (dhcp_supplied_address(interface)) {
        copy_ip4_bytes(address, netif_ip4_addr(interface));
        copy_ip4_bytes(netmask, netif_ip4_netmask(interface));
        copy_ip4_bytes(gateway, netif_ip4_gw(interface));
        dns_server = dns_getserver(0);
        if (dns_server && IP_IS_V4(dns_server))
            copy_ip4_bytes(dns, ip_2_ip4(dns_server));
        (void)netdev_set_ipv4(index, address, netmask, gateway, dns,
                              NETDEV_DHCP_BOUND);
        return;
    }
    if (info.dhcp_state != NETDEV_DHCP_IDLE &&
        info.dhcp_state != NETDEV_DHCP_DISCOVERING)
        state = (netdev_dhcp_state_t)info.dhcp_state;
    (void)netdev_set_ipv4(index, NULL, NULL, NULL, NULL, state);
}

void net_stack_poll(void)
{
    uint8_t frame[NETDEV_MAX_FRAME_SIZE];
    if (!stack_initialized)
        net_stack_init();
    if (!stack_initialized)
        return;

    for (int index = 0; index < NETDEV_MAX_DEVICES; index++) {
        struct netif* interface;
        netdev_info_t info;
        if (!netdev_get_info(index, &info))
            continue;
        interface = lwip_port_netif(index);
        if (!interface) {
            lwip_port_poll();
            interface = lwip_port_netif(index);
        }
        if (!interface)
            continue;
        for (unsigned int count = 0; count < NET_POLL_FRAMES; count++) {
            int length = netdev_receive(index, frame, sizeof(frame));
            if (length <= 0)
                break;
            (void)lwip_port_receive(index, frame, (size_t)length);
        }
        update_device_configuration(index, interface);
    }
    lwip_port_poll();
}
