/**
 * @file ethernet.c
 * @brief Ethernet (Layer 2) frame processing.
 */

#include "ethernet.h"
#include "netdev.h"
#include "arp.h"
#include "ip.h"
#include "serial.h"
#include "string.h"
#include "endian.h"

// ── Well-known MAC addresses ───────────────────────────────────────────

const uint8_t ETH_BROADCAST[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const uint8_t ETH_ZERO[ETH_ALEN]      = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// ── MAC address formatting ─────────────────────────────────────────────

char *eth_addr_to_str(const uint8_t *addr, char *buf)
{
    // Static buffer for single-use in kprintf (not reentrant)
    static char static_buf[18];
    char *out = buf ? buf : static_buf;

    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < ETH_ALEN; ++i)
    {
        out[i * 3]     = hex[addr[i] >> 4];
        out[i * 3 + 1] = hex[addr[i] & 0x0F];
        out[i * 3 + 2] = (i < 5) ? ':' : '\0';
    }

    return out;
}

// ── RX path ────────────────────────────────────────────────────────────

void eth_input(struct netbuf *nb)
{
    if (!nb)
        return;

    if (nb->len < ETH_HLEN)
    {
        kprintf("[ETH] Runt frame: %lu bytes (need %u)\n",
                (uint64_t)nb->len, ETH_HLEN);
        netbuf_free(nb);
        return;
    }

    // Parse the Ethernet header
    struct ethhdr *eth = (struct ethhdr *)nb->data;
    nb->mac_header = eth;

    // Get our MAC address for destination matching
    uint8_t our_mac[ETH_ALEN];
    if (nb->dev)
        netdev_get_mac(nb->dev, our_mac);
    else
        memset(our_mac, 0, ETH_ALEN);

    // Classify the packet type
    if (eth_is_broadcast(eth->dest))             nb->pkt_type = PKT_BROADCAST;
    else if (eth_is_multicast(eth->dest))        nb->pkt_type = PKT_MULTICAST;
    else if (eth_addr_equal(eth->dest, our_mac)) nb->pkt_type = PKT_HOST;
    else
    {
        nb->pkt_type = PKT_OTHERHOST;
        netbuf_free(nb);
        return;
    }

    // Extract ethertype and strip the Ethernet header
    uint16_t ethertype = ntohs(eth->ethertype);
    nb->protocol = ethertype;

    if (!netbuf_pull(nb, ETH_HLEN))
    {
        netbuf_free(nb);
        return;
    }

    // Dispatch to the appropriate L3 handler
    switch (ethertype)
    {
        case ETH_P_ARP:
            arp_input(nb);
            break;
        case ETH_P_IP:
            ip_input(nb);
            break;
        default:
            kprintf("[ETH] Unknown ethertype 0x%x; dropping\n", ethertype);
            netbuf_free(nb);
            break;
    }
}

// ── TX path ────────────────────────────────────────────────────────────

void eth_build_header(struct netbuf *nb, const uint8_t *dest,
                      const uint8_t *src, uint16_t ethertype)
{
    struct ethhdr *eth = (struct ethhdr *)netbuf_push(nb, ETH_HLEN);
    if (!eth)
        return;

    eth_copy_addr(eth->dest, dest);
    eth_copy_addr(eth->src, src);
    eth->ethertype = htons(ethertype);
    nb->mac_header = eth;
}

int eth_output(struct netbuf *nb, const uint8_t *dest, uint16_t ethertype)
{
    if (!nb)
        return -1;

    struct netdev *dev = nb->dev;
    if (!dev)
    {
        kprintf("[ETH] No device set for output\n");
        netbuf_free(nb);
        return -1;
    }

    // Build the Ethernet header using the device's MAC as source
    uint8_t src_mac[ETH_ALEN];
    netdev_get_mac(dev, src_mac);
    eth_build_header(nb, dest, src_mac, ethertype);

    // Check that the header push succeeded
    if (!nb->mac_header)
    {
        kprintf("[ETH] Failed to push Ethernet header\n");
        netbuf_free(nb);
        return -1;
    }

    // Pad to minimum Ethernet frame size (hardware may also do this)
    if (nb->len < ETH_FRAME_MIN)
    {
        size_t pad = ETH_FRAME_MIN - nb->len;
        void *p = netbuf_put(nb, pad);
        if (p)
            memset(p, 0, pad);
    }

    // Transmit via the network device
    int ret = netdev_send(dev, nb);
    if (ret != 0)
        netbuf_free(nb);

    return ret;
}

