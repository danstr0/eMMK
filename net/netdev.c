/**
 * @file netdev.c
 * @brief Network device subsystem.
 */

#include "netdev.h"
#include "ethernet.h"
#include "serial.h"
#include "string.h"
#include "heap.h"

// ── Device table ───────────────────────────────────────────────────────

static struct netdev *devices[NETDEV_MAX];
static int device_count = 0;

// ── Initialization ─────────────────────────────────────────────────────

void netdev_init(void)
{
    memset(devices, 0, sizeof(devices));
    device_count = 0;
    kprintf("[NETDEV] Subsystem initialized\n");
}

struct netdev *netdev_alloc(const char *name)
{
    struct netdev *dev = kzalloc(sizeof(struct netdev));
    if (!dev)
        return NULL;

    if (name)
        strlcpy(dev->name, name, sizeof(dev->name));

    dev->mtu = ETH_MTU;
    netbuf_queue_init(&dev->rx_queue);

    return dev;
}

// ── Registration ───────────────────────────────────────────────────────

int netdev_register(struct netdev *dev)
{
    if (!dev || device_count >= NETDEV_MAX)
    {
        kprintf("[NETDEV] Registration failed (table full or NULL dev)\n");
        return -1;
    }

    // Assign a default name if none was provided
    if (dev->name[0] == '\0')
    {
        // Simple single-digit scheme; safe because NETDEV_MAX < 10
        dev->name[0] = 'e';
        dev->name[1] = 't';
        dev->name[2] = 'h';
        dev->name[3] = '0' + (char)device_count;
        dev->name[4] = '\0';
    }

    netbuf_queue_init(&dev->rx_queue);

    if (dev->mtu == 0)
        dev->mtu = ETH_MTU;

    devices[device_count++] = dev;

    kprintf("[NETDEV] Registered %s: MAC=%s, MTU=%lu\n",
            dev->name, ETH_FMT(dev->mac), (uint64_t)dev->mtu);

    return 0;
}

void netdev_unregister(struct netdev *dev)
{
    if (!dev)
        return;

    for (int i = 0; i < device_count; ++i)
        if (devices[i] == dev)
        {
            // Purge any queued RX packets
            netbuf_queue_purge(&dev->rx_queue);

            // Compact the device table
            for (int j = i; j < device_count - 1; ++j)
                devices[j] = devices[j + 1];
            device_count--;
            devices[device_count] = NULL;

            kprintf("[NETDEV] Unregistered %s\n", dev->name);
            return;
        }
}

// ── Lookup ─────────────────────────────────────────────────────────────

struct netdev *netdev_find(const char *name)
{
    if (!name)
        return NULL;
    for (int i = 0; i < device_count; ++i)
        if (strcmp(devices[i]->name, name) == 0)
            return devices[i];
    return NULL;
}

struct netdev *netdev_get_default(void)
{
    return device_count > 0 ? devices[0] : NULL;
}

// ── Data path - transmit ───────────────────────────────────────────────

int netdev_send(struct netdev *dev, struct netbuf *nb)
{
    if (!dev || !nb)
        return -1;

    if (!netdev_is_up(dev))
    {
        dev->stats.tx_dropped++;
        netbuf_free(nb);
        return -1;
    }

    if (!dev->ops || !dev->ops->send)
    {
        dev->stats.tx_dropped++;
        netbuf_free(nb);
        return -1;
    }

    // Save the length before calling send — the driver takes ownership
    // of nb and may free it before returning
    uint32_t pkt_len = nb->len;

    int ret = dev->ops->send(dev, nb);

    if (ret == 0)
    {
        dev->stats.tx_packets++;
        dev->stats.tx_bytes += pkt_len;
    }
    else
        dev->stats.tx_errors++;

    return ret;
}

// ── Data path - receive ────────────────────────────────────────────────

void netdev_receive(struct netdev *dev, struct netbuf *nb)
{
    if (!dev || !nb)
        return;

    nb->dev = dev;
    dev->stats.rx_packets++;
    dev->stats.rx_bytes += nb->len;

    netbuf_queue_push(&dev->rx_queue, nb);
}

// ── Polling ────────────────────────────────────────────────────────────

void netdev_poll(struct netdev *dev)
{
    if (!dev || !netdev_is_up(dev))
        return;
    if (dev->ops && dev->ops->poll)
        dev->ops->poll(dev);
}

void netdev_poll_all(void)
{
    for (int i = 0; i < device_count; ++i)
        netdev_poll(devices[i]);
}

void netdev_process_rx(void)
{
    for (int i = 0; i < device_count; ++i)
    {
        struct netdev *dev = devices[i];
        struct netbuf *nb;

        while ((nb = netbuf_queue_pop(&dev->rx_queue)) != NULL)
            eth_input(nb);
    }
}

// ── IP configuration ───────────────────────────────────────────────────

void netdev_set_ip(struct netdev *dev, uint32_t ip, uint32_t netmask,
                   uint32_t gateway)
{
    if (!dev)
        return;

    dev->ip_addr = ip;
    dev->netmask = netmask;
    dev->gateway = gateway;

    kprintf("[NETDEV] %s: IP=%s", dev->name, IP4_FMT(ip));
    kprintf(" netmask=%s", IP4_FMT(netmask));
    kprintf(" gateway=%s\n", IP4_FMT(gateway));
}

// ── IP address formatting ──────────────────────────────────────────────

char *ip4_addr_to_str(uint32_t ip, char *buf)
{
    static char static_buf[16];
    char *out = buf ? buf : static_buf;

    // Host byte order: MSB = first octet
    uint8_t a = (ip >> 24) & 0xFF;
    uint8_t b = (ip >> 16) & 0xFF;
    uint8_t c = (ip >> 8)  & 0xFF;
    uint8_t d =  ip        & 0xFF;

    // Simple decimal formatting without sprintf
    int pos = 0;
    for (int i = 0; i < 4; ++i)
    {
        uint8_t octet = (i == 0) ? a : (i == 1) ? b : (i == 2) ? c : d;
        if (octet >= 100) out[pos++] = '0' + octet / 100;
        if (octet >= 10)  out[pos++] = '0' + (octet / 10) % 10;
        out[pos++] = '0' + octet % 10;
        if (i < 3) out[pos++] = '.';
    }
    out[pos] = '\0';

    return out;
}

