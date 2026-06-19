/**
 * @file icmp.c
 * @brief ICMP implementation.
 */

#include "icmp.h"
#include "ip.h"
#include "netdev.h"
#include "ethernet.h"
#include "endian.h"
#include "serial.h"
#include "string.h"

struct icmp_stats icmp_statistics;

// ── Initialization ─────────────────────────────────────────────────────

void icmp_init(void)
{
    memset(&icmp_statistics, 0, sizeof(icmp_statistics));
    kprintf("[ICMP] Initialized\n");
}

void icmp_get_stats(struct icmp_stats *stats)
{
    if (stats)
        *stats = icmp_statistics;
}

// ── Checksum (wrapper around the IP ones-complement checksum) ──────────

static inline uint16_t icmp_checksum(const void *data, size_t len)
{
    return ip_checksum(data, len);
}

// ── Echo reply (ping response) ─────────────────────────────────────────

void icmp_send_echo_reply(struct netbuf *nb)
{
    if (!nb)
        return;

    // The buffer contains the ICMP header + payload (IP header was
    // stripped by ip_input). Modify the ICMP header in place: change
    // the type to REPLY and recompute the checksum.
    //
    // ip_output() will push a new IP header using the device's IP as
    // source and the original sender as destination.
    struct icmphdr *icmp = (struct icmphdr *)nb->data;

    // The reply goes back to the original sender
    uint32_t reply_dst = nb->src_ip;

    // Convert request to reply
    icmp->type = ICMP_ECHO_REPLY;
    icmp->code = 0;

    // Recalculate checksum over the modified header + payload
    icmp->checksum = 0;
    icmp->checksum = icmp_checksum(icmp, nb->len);

    // Clear the device so ip_output() does a fresh route lookup
    // This ensures the reply goes out the correct interface
    nb->dev = NULL;

    // ip_output always consumes nb
    if (ip_output(nb, reply_dst, IPPROTO_ICMP) == 0)
        icmp_statistics.tx_echo_reply++;
}

// ── RX dispatch ────────────────────────────────────────────────────────

void icmp_input(struct netbuf *nb)
{
    if (!nb)
        return;

    if (nb->len < ICMP_HDR_SIZE)
    {
        icmp_statistics.rx_errors++;
        netbuf_free(nb);
        return;
    }

    struct icmphdr *icmp = (struct icmphdr *)nb->data;

    // Verify checksum over the entire ICMP message
    if (icmp_checksum(icmp, nb->len) != 0)
    {
        icmp_statistics.checksum_errors++;
        netbuf_free(nb);
        return;
    }

    switch (icmp->type)
    {
        case ICMP_ECHO_REQUEST:
            icmp_statistics.rx_echo_request++;
            kprintf("[ICMP] Echo request from %s, id=%u seq=%u\n",
                    IP4_FMT(nb->src_ip),
                    ntohs(icmp->un.echo.id),
                    ntohs(icmp->un.echo.sequence));
            // icmp_send_echo_reply takes ownership of nb
            icmp_send_echo_reply(nb);
            break;
        case ICMP_ECHO_REPLY:
            icmp_statistics.rx_echo_reply++;
            kprintf("[ICMP] Echo reply from %s, id=%u seq=%u\n",
                    IP4_FMT(nb->src_ip),
                    ntohs(icmp->un.echo.id),
                    ntohs(icmp->un.echo.sequence));
            netbuf_free(nb);
            break;
        case ICMP_DEST_UNREACHABLE:
            kprintf("[ICMP] Destination unreachable from %s (code %u)\n",
                    IP4_FMT(nb->src_ip), icmp->code);
            netbuf_free(nb);
            break;
        case ICMP_TIME_EXCEEDED:
            kprintf("[ICMP] Time exceeded from %s (code %u)\n",
                    IP4_FMT(nb->src_ip), icmp->code);
            netbuf_free(nb);
            break;
        default:
            kprintf("[ICMP] Unknown type %u from %s\n",
                    icmp->type, IP4_FMT(nb->src_ip));
            netbuf_free(nb);
            break;
    }
}

// ── Echo request (active ping) ─────────────────────────────────────────

int icmp_send_echo_request(uint32_t dst_ip, uint16_t id, uint16_t seq,
                           const void *data, size_t len)
{
    struct netbuf *nb = netbuf_alloc(0);
    if (!nb)
        return -1;

    // Reserve space for Ethernet + IP headers
    netbuf_reserve(nb, ETH_HLEN + IP_HDR_MIN);

    // Build the ICMP echo request header
    struct icmphdr *icmp = (struct icmphdr *)netbuf_put(nb, ICMP_HDR_SIZE);
    if (!icmp)
    {
        netbuf_free(nb);
        return -1;
    }

    icmp->type             = ICMP_ECHO_REQUEST;
    icmp->code             = 0;
    icmp->checksum         = 0;
    icmp->un.echo.id       = htons(id);
    icmp->un.echo.sequence = htons(seq);

    // Append optional payload
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

    // Checksum covers the entire ICMP message (header + payload)
    icmp->checksum = icmp_checksum(icmp, ICMP_HDR_SIZE + len);

    icmp_statistics.tx_echo_request++;

    kprintf("[ICMP] Sending echo request to %s, id=%u seq=%u\n",
            IP4_FMT(dst_ip), id, seq);

    // ip_output always consumes nb
    return ip_output(nb, dst_ip, IPPROTO_ICMP);
}

// ── Destination unreachable ────────────────────────────────────────────

void icmp_send_dest_unreachable(struct netbuf *nb, uint8_t code)
{
    if (!nb)
        return;

    // Per RFC 792, ICMP error messages include the original IP header
    // plus the first 8 bytes of the original datagram's payload.
    // nb->network_header must point at the original IP header.
    struct iphdr *orig_ip = (struct iphdr *)nb->network_header;
    if (!orig_ip)
        return;

    size_t orig_hdr_len = (size_t)orig_ip->ihl * 4;
    size_t include_len  = orig_hdr_len + 8;

    // Calculate how much of the original packet is actually available
    size_t available = (size_t)(nb->tail - (uint8_t *)orig_ip);
    if (include_len > available)
        include_len = available;

    struct netbuf *reply = netbuf_alloc(0);
    if (!reply)
        return;

    netbuf_reserve(reply, ETH_HLEN + IP_HDR_MIN);

    // Build the ICMP header
    struct icmphdr *icmp = (struct icmphdr *)netbuf_put(reply, ICMP_HDR_SIZE);
    if (!icmp)
    {
        netbuf_free(reply);
        return;
    }

    icmp->type        = ICMP_DEST_UNREACHABLE;
    icmp->code        = code;
    icmp->checksum    = 0;
    icmp->un.raw      = 0;  // unused for most unreachable codes

    // Copy the original IP header + first 8 bytes of payload
    void *p = netbuf_put(reply, include_len);
    if (!p)
    {
        netbuf_free(reply);
        return;
    }
    memcpy(p, orig_ip, include_len);

    // Checksum over the complete ICMP message
    icmp->checksum = icmp_checksum(icmp, ICMP_HDR_SIZE + include_len);

    icmp_statistics.tx_dest_unreachable++;

    // Send back to the original sender
    uint32_t dst_ip = ntohl(orig_ip->saddr);
    ip_output(reply, dst_ip, IPPROTO_ICMP);
}

