/**
 * @file ip.c
 * @brief IPv4 implementation. 
 */

#include "ip.h"
#include "netdev.h"
#include "ethernet.h"
#include "arp.h"
#include "icmp.h"
#include "endian.h"
#include "serial.h"
#include "string.h"

// ── State ──────────────────────────────────────────────────────────────

/// Monotonically increasing IP identification field.
static uint16_t ip_id_counter = 0;

// ── Initialization ─────────────────────────────────────────────────────

void ip_init(void)
{
    ip_id_counter = 0;
    kprintf("[IP] Initialized\n");
}

// ── Checksum ───────────────────────────────────────────────────────────

uint16_t ip_checksum(const void *data, size_t len)
{
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;

    while (len > 1)
    {
        sum += *ptr++;
        len -= 2;
    }

    // Handle a trailing odd byte
    if (len == 1)
        sum += *(const uint8_t *)ptr;

    // Fold the 32-bit accumulator into 16 bits
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

// ── Address classification ─────────────────────────────────────────────

bool ip_is_local(uint32_t ip)
{
    // Loopback range: 127.0.0.0/8
    if ((ip >> 24) == 127)
        return true;

    // Check the default device. Since eMMK is currently single-NIC
    // this is sufficient - if support for multi-NIC is added, iterate
    // all registered netdevs.
    struct netdev *dev = netdev_get_default();
    if (dev && dev->ip_addr != 0 && dev->ip_addr == ip)
        return true;

    return false;
}

bool ip_is_broadcast(uint32_t ip, struct netdev *dev)
{
    // Limited broadcast
    if (ip == 0xFFFFFFFF)
        return true;

    // Subnet-directed broadcast
    if (dev && dev->netmask != 0)
    {
        uint32_t net   = dev->ip_addr & dev->netmask;
        uint32_t bcast = net | ~dev->netmask;
        if (ip == bcast)
            return true;
    }

    return false;
}

// ── Routing ────────────────────────────────────────────────────────────

int ip_route(uint32_t dst_ip, struct netdev **dev, uint32_t *next_hop)
{
    struct netdev *out_dev = netdev_get_default();
    if (!out_dev || out_dev->ip_addr == 0)
        return -1;

    *dev = out_dev;

    // Broadcast: deliver directly
    if (ip_is_broadcast(dst_ip, out_dev))
    {
        *next_hop = dst_ip;
        return 0;
    }

    // On-link: destination is on this host's subnet
    if ((dst_ip & out_dev->netmask)
        == (out_dev->ip_addr & out_dev->netmask))
    {
        *next_hop = dst_ip;
        return 0;
    }

    // Off-link: use the default gateway
    if (out_dev->gateway != 0)
    {
        *next_hop = out_dev->gateway;
        return 0;
    }

    kprintf("[IP] No route to %s\n", IP4_FMT(dst_ip));
    return -1;
}

// ── RX path ────────────────────────────────────────────────────────────

void ip_input(struct netbuf *nb)
{
    if (!nb)
        return;

    if (nb->len < IP_HDR_MIN)
    {
        kprintf("[IP] Packet too short: %lu bytes\n", (uint64_t)nb->len);
        netbuf_free(nb);
        return;
    }

    struct iphdr *ip = (struct iphdr *)nb->data;

    // Validate version
    if (ip->version != IP_VERSION)
    {
        netbuf_free(nb);
        return;
    }

    // Validate header length
    size_t hdr_len = (size_t)ip->ihl * 4;
    if (hdr_len < IP_HDR_MIN || hdr_len > nb->len)
    {
        netbuf_free(nb);
        return;
    }

    // Verify header checksum
    if (ip_checksum(ip, hdr_len) != 0)
    {
        kprintf("[IP] Bad checksum, dropping\n");
        netbuf_free(nb);
        return;
    }

    // Validate total length
    uint16_t tot_len = ntohs(ip->tot_len);
    if (tot_len < hdr_len || tot_len > nb->len)
    {
        netbuf_free(nb);
        return;
    }

    // Trim the buffer to the IP total length (Ethernet frames may be
    // padded to 60 bytes; the IP total length gives the real size)
    nb->network_header = ip;
    netbuf_set_len(nb, tot_len);

    // Extract addresses (network → host byte order)
    uint32_t src_ip = ntohl(ip->saddr);
    uint32_t dst_ip = ntohl(ip->daddr);

    nb->src_ip   = src_ip;
    nb->dst_ip   = dst_ip;
    nb->ip_proto = ip->protocol;

    // No forwarding - drop packets not addressed to this host
    if (!ip_is_local(dst_ip) && !ip_is_broadcast(dst_ip, nb->dev))
    {
        netbuf_free(nb);
        return;
    }

    // Strip the IP header to expose the L4 payload
    if (!netbuf_pull(nb, hdr_len))
    {
        netbuf_free(nb);
        return;
    }
    nb->transport_header = nb->data;

    // Dispatch to L4 protocol handler
    switch (ip->protocol)
    {
        case IPPROTO_ICMP:
            icmp_input(nb);
            break; 
        // TCP and UDP are not yet implemented - drop
        case IPPROTO_TCP:
        case IPPROTO_UDP:
        default:
            netbuf_free(nb);
            break;
    }
}

// ── TX path ────────────────────────────────────────────────────────────

int ip_output(struct netbuf *nb, uint32_t dst_ip, uint8_t protocol)
{
    if (!nb)
        return -1;

    // Route the packet: determine device and next-hop
    struct netdev *dev;
    uint32_t next_hop;

    if (ip_route(dst_ip, &dev, &next_hop) < 0)
    {
        netbuf_free(nb);
        return -1;
    }

    nb->dev = dev;

    // Push the IP header
    struct iphdr *ip = (struct iphdr *)netbuf_push(nb, IP_HDR_MIN);
    if (!ip)
    {
        netbuf_free(nb);
        return -1;
    }

    // Fill in the header fields
    ip->version  = IP_VERSION;
    ip->ihl      = IP_HDR_MIN / 4;  // 5 (no options)
    ip->tos      = 0;
    ip->tot_len  = htons(nb->len);
    ip->id       = htons(ip_id_counter++);
    ip->frag_off = htons(IP_FLAG_DF);
    ip->ttl      = IP_DEFAULT_TTL;
    ip->protocol = protocol;
    ip->check    = 0;
    ip->saddr    = htonl(dev->ip_addr);
    ip->daddr    = htonl(dst_ip);

    // Compute the header checksum (after all fields are set)
    ip->check = ip_checksum(ip, IP_HDR_MIN);

    nb->network_header = ip;
    nb->dst_ip = dst_ip;

    // Resolve the next-hop MAC address
    uint8_t dst_mac[ETH_ALEN];
    int arp_result = arp_resolve(dev, next_hop, dst_mac);

    switch (arp_result)
    {
        case ARP_OK:
            return ip_output_resolved(nb, dst_mac);
        case ARP_PENDING:
            return arp_queue_packet(dev, next_hop, nb);
        default:
            netbuf_free(nb);
            return -1;
    }
}

int ip_output_resolved(struct netbuf *nb, const uint8_t *dst_mac)
{
    return eth_output(nb, dst_mac, ETH_P_IP);
}

