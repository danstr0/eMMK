/**
 * @file udp.c
 * @brief UDP implementation.
 *
 * @par Checksum
 * The UDP checksum covers a 12-byte pseudo-header (source IP,
 * destination IP, zero, protocol, UDP length) concatenated with the
 * UDP header and payload. It uses the same ones-complement algorithm
 * as the IP header checksum.
 *
 * On TX, a computed checksum of @c 0x0000 is transmitted as @c 0xFFFF
 * (per RFC 768). On RX, a received checksum of @c 0x0000 means the
 * sender did not compute it (valid for IPv4).
 */

#include "udp.h"
#include "ip.h"
#include "icmp.h"
#include "netdev.h"
#include "ethernet.h"
#include "endian.h"
#include "serial.h"
#include "string.h"
#include "spinlock.h"

// ── Bind table ─────────────────────────────────────────────────────────

struct udp_bind_entry
{
    uint16_t        port;     ///< Port number (host byte order).
    udp_recv_func_t handler;  ///< Receive callback.
    bool            in_use;
};

static struct udp_bind_entry bind_table[UDP_MAX_BINDS];

static spinlock_t udp_bind_lock = SPINLOCK_INIT;

// ── Checksum ───────────────────────────────────────────────────────────

/**
 * @brief Compute the UDP checksum over pseudo-header + UDP message.
 *
 * @param udp_data    Start of the UDP header (header + payload).
 * @param udp_len     Total UDP length (header + payload) in bytes.
 * @param src_ip_net  Source IP in @e network byte order.
 * @param dst_ip_net  Destination IP in @e network byte order.
 * @return Ones-complement checksum (@c 0 = valid when verifying).
 */
static uint16_t udp_checksum(const void *udp_data, size_t udp_len,
                             uint32_t src_ip_net, uint32_t dst_ip_net)
{
    uint32_t sum = 0;

    // Pseudo-header (all values already in network byte order)
    sum += (src_ip_net >> 16) & 0xFFFF;
    sum += src_ip_net & 0xFFFF;
    sum += (dst_ip_net >> 16) & 0xFFFF;
    sum += dst_ip_net & 0xFFFF;
    sum += htons(IPPROTO_UDP);
    sum += htons((uint16_t)udp_len);

    // UDP header + payload
    const uint16_t *ptr = (const uint16_t *)udp_data;
    size_t remaining = udp_len;

    while (remaining > 1)
    {
        sum += *ptr++;
        remaining -= 2;
    }

    // Trailing odd byte
    if (remaining == 1)
        sum += *(const uint8_t *)ptr;

    // Fold 32-bit accumulator into 16 bits
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

// ── Initialization ─────────────────────────────────────────────────────

void udp_init(void)
{
    memset(bind_table, 0, sizeof(bind_table));
    kprintf("[UDP] Initialized (%d bind slots)\n", UDP_MAX_BINDS);
}

// ── Port binding ───────────────────────────────────────────────────────

int udp_bind(uint16_t port, udp_recv_func_t handler)
{
    if (!handler || port == 0)
        return -1;

    uint64_t flags;
    spin_lock_irqsave(&udp_bind_lock, &flags);

    // Reject duplicate bindings
    for (int i = 0; i < UDP_MAX_BINDS; ++i)
        if (bind_table[i].in_use && bind_table[i].port == port)
        {
            spin_unlock_irqrestore(&udp_bind_lock, flags);
            return -1;
        }

    // Find a free slot
    for (int i = 0; i < UDP_MAX_BINDS; ++i)
        if (!bind_table[i].in_use)
        {
            bind_table[i].port    = port;
            bind_table[i].handler = handler;
            bind_table[i].in_use  = true;
            spin_unlock_irqrestore(&udp_bind_lock, flags);
            kprintf("[UDP] Bound port %u\n", port);
            return 0;
        }

    spin_unlock_irqrestore(&udp_bind_lock, flags);
    kprintf("[UDP] Bind failed: table full\n");
    return -1;
}

void udp_unbind(uint16_t port)
{
    uint64_t flags;
    spin_lock_irqsave(&udp_bind_lock, &flags);

    for (int i = 0; i < UDP_MAX_BINDS; ++i)
        if (bind_table[i].in_use && bind_table[i].port == port)
        {
            bind_table[i].in_use  = false;
            bind_table[i].handler = NULL;
            spin_unlock_irqrestore(&udp_bind_lock, flags);
            kprintf("[UDP] Unbound port %u\n", port);
            return;
        }

    spin_unlock_irqrestore(&udp_bind_lock, flags);
}

/// Look up the handler for a destination port.
static udp_recv_func_t udp_find_handler(uint16_t port)
{
    uint64_t flags;
    spin_lock_irqsave(&udp_bind_lock, &flags);

    for (int i = 0; i < UDP_MAX_BINDS; ++i)
        if (bind_table[i].in_use && bind_table[i].port == port)
        {
            udp_recv_func_t h = bind_table[i].handler;
            spin_unlock_irqrestore(&udp_bind_lock, flags);
            return h;
        }

    spin_unlock_irqrestore(&udp_bind_lock, flags);
    return NULL;
}

// ── RX path ────────────────────────────────────────────────────────────

void udp_input(struct netbuf *nb)
{
    if (!nb)
        return;

    if (nb->len < UDP_HDR_SIZE)
    {
        kprintf("[UDP] Packet too short: %lu bytes\n", (uint64_t)nb->len);
        netbuf_free(nb);
        return;
    }

    struct udphdr *udp = (struct udphdr *)nb->data;

    uint16_t src_port = ntohs(udp->src_port);
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t udp_len  = ntohs(udp->len);

    // Validate the length field
    if (udp_len < UDP_HDR_SIZE || udp_len > nb->len)
    {
        netbuf_free(nb);
        return;
    }

    // Trim to the UDP length (strip any trailing IP padding)
    if (udp_len < nb->len)
        netbuf_set_len(nb, udp_len);

    // Verify checksum if present (0 = sender did not compute it)
    if (udp->checksum != 0)
    {
        struct iphdr *ip = (struct iphdr *)nb->network_header;
        if (ip)
        {
            uint16_t csum = udp_checksum(udp, udp_len,
                                         ip->saddr, ip->daddr);
            if (csum != 0)
            {
                kprintf("[UDP] Bad checksum, dropping\n");
                netbuf_free(nb);
                return;
            }
        }
    }

    // Populate addressing metadata
    nb->src_port = src_port;
    nb->dst_port = dst_port;

    // Save transport header, then strip the UDP header
    nb->transport_header = udp;
    if (!netbuf_pull(nb, UDP_HDR_SIZE))
    {
        netbuf_free(nb);
        return;
    }

    // Dispatch to the bound handler
    udp_recv_func_t handler = udp_find_handler(dst_port);
    if (handler)
    {
        handler(nb);  // handler takes ownership
        return;
    }

    // No handler - send ICMP Port Unreachable
    //
    // Push the UDP header back so the ICMP error message includes
    // the original IP header + first 8 bytes of transport data
    // (the UDP header itself)
    netbuf_push(nb, UDP_HDR_SIZE);
    icmp_send_dest_unreachable(nb, ICMP_PORT_UNREACHABLE);
    netbuf_free(nb);
}

// ── TX path ────────────────────────────────────────────────────────────

int udp_output(struct netbuf *nb, uint32_t dst_ip,
               uint16_t dst_port, uint16_t src_port)
{
    if (!nb)
        return -1;

    uint16_t payload_len = (uint16_t)nb->len;
    uint16_t udp_len     = UDP_HDR_SIZE + payload_len;

    // Push the UDP header
    struct udphdr *udp = (struct udphdr *)netbuf_push(nb, UDP_HDR_SIZE);
    if (!udp)
    {
        netbuf_free(nb);
        return -1;
    }

    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->len      = htons(udp_len);
    udp->checksum = 0;

    // Compute the checksum. The source IP comes from the outgoing
    // device; ip_output() will fill in the actual IP header field.
    struct netdev *dev = netdev_get_default();
    uint32_t src_ip = dev ? dev->ip_addr : 0;

    uint16_t csum = udp_checksum(udp, udp_len,
                                 htonl(src_ip), htonl(dst_ip));
    // RFC 768: a computed checksum of 0 is transmitted as 0xFFFF
    udp->checksum = (csum == 0) ? 0xFFFF : csum;

    nb->transport_header = udp;

    // ip_output always consumes nb
    return ip_output(nb, dst_ip, IPPROTO_UDP);
}

int udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
             const void *data, size_t len)
{
    struct netbuf *nb = netbuf_alloc(0);
    if (!nb)
        return -1;

    // Reserve headroom for Ethernet + IP + UDP headers
    netbuf_reserve(nb, ETH_HLEN + IP_HDR_MIN + UDP_HDR_SIZE);

    // Copy the payload
    if (data && len > 0)
    {
        void *p = netbuf_put(nb, len);
        if (!p)
        {
            netbuf_free(nb);
            return -1;
        }
        memcpy(p, data, len);
    }

    return udp_output(nb, dst_ip, dst_port, src_port);
}

