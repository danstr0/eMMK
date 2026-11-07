/**
 * @file netdev.c
 * @brief Network device subsystem.
 */

#include "netdev.h"
#include "ethernet.h"
#include "serial.h"
#include "string.h"
#include "heap.h"
#include "format.h"

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
    dev->num_queues = 1;

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

    // Assign the lowest free ethN if no name was provided
    if (dev->name[0] == '\0')
        for (int n = 0; n < NETDEV_MAX; ++n)
        {
            ksnprintf(dev->name, sizeof(dev->name), "eth%d", n);
            if (!netdev_find(dev->name))
                break;
        }

    if (netdev_find(dev->name))
    {
        kprintf("[NETDEV] Registration failed: %s already exists\n",
                dev->name);
        return -1;
    }

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
            // Drop anything still held back for transmission
            while (dev->tx_held > 0)
                netbuf_free(dev->tx_hold[--dev->tx_held]);

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

int netdev_count(void)
{
    return device_count;
}

struct netdev *netdev_get(int index)
{
    if (index < 0 || index >= device_count)
        return NULL;
    return devices[index];
}

// ── Data path - transmit ───────────────────────────────────────────────

/// Inside a netdev_tx_defer_begin() section (written by the TX owner only).
static bool tx_deferred = false;

uint16_t netdev_tx_burst(struct netdev *dev, uint16_t qid,
                         struct netbuf **pkts, uint16_t n)
{
    if (!dev->ops || !dev->ops->tx_burst)
        return 0;

    if (n > NETDEV_BURST_MAX)
        n = NETDEV_BURST_MAX;

    // Read the lengths first
    uint32_t lens[NETDEV_BURST_MAX];
    for (uint16_t i = 0; i < n; ++i)
        lens[i] = pkts[i]->len;

    uint16_t sent = dev->ops->tx_burst(dev, qid, pkts, n);

    dev->stats.tx_packets += sent;
    for (uint16_t i = 0; i < sent; ++i)
        dev->stats.tx_bytes += lens[i];

    return sent;
}

/// Post everything held back on @p dev; drop what the driver refuses.
static void tx_flush(struct netdev *dev)
{
    uint16_t held = dev->tx_held;
    uint16_t sent = netdev_tx_burst(dev, 0, dev->tx_hold, held);

    for (uint16_t i = sent; i < held; ++i)
    {
        dev->stats.tx_dropped++;
        netbuf_free(dev->tx_hold[i]);
    }
    dev->tx_held = 0;
}

int netdev_send(struct netdev *dev, struct netbuf *nb)
{
    if (!dev || !nb)
        return -1;

    if (!netdev_is_up(dev) || !dev->ops || !dev->ops->tx_burst )
    {
        dev->stats.tx_dropped++;
        netbuf_free(nb);
        return -1;
    }

    if (tx_deferred)
    {
        if (dev->tx_held == NETDEV_BURST_MAX)
            tx_flush(dev);
        dev->tx_hold[dev->tx_held++] = nb;
        return 0;
    }

    if (netdev_tx_burst(dev, 0, &nb, 1) == 1)
        return 0;

    dev->stats.tx_dropped++;
    netbuf_free(nb);
    return -1;
}

void netdev_tx_defer_begin(void)
{
    tx_deferred = true;
}

void netdev_tx_defer_end(void)
{
    tx_deferred = false;

    for (int i = 0; i < device_count; ++i)
        if (devices[i]->tx_held > 0)
            tx_flush(devices[i]);
}

// ── Data path - receive ────────────────────────────────────────────────

uint16_t netdev_rx_burst(struct netdev *dev, uint16_t qid,
                         struct netbuf **pkts, uint16_t max)
{
    if (!netdev_is_up(dev) || !dev->ops || !dev->ops->rx_burst)
        return 0;

    uint16_t n = dev->ops->rx_burst(dev, qid, pkts, max);

    for (uint16_t i = 0; i < n; ++i)
    {
        pkts[i]->dev = dev;
        dev->stats.rx_bytes += pkts[i]->len;
    }
    dev->stats.rx_packets += n;

    return n;
}

// ── Polling ────────────────────────────────────────────────────────────

uint16_t netdev_poll(struct netdev *dev)
{
    struct netbuf *pkts[NETDEV_BURST_MAX];
    uint16_t n = netdev_rx_burst(dev, 0, pkts, NETDEV_BURST_MAX);

    for (uint16_t i = 0; i < n; ++i)
        eth_input(pkts[i]);

    return n;
}

uint32_t netdev_poll_all(void)
{
    uint32_t total = 0;

    netdev_tx_defer_begin();
    for (int i = 0; i < device_count; ++i)
        total += netdev_poll(devices[i]);
    netdev_tx_defer_end();

    return total;
}

// ── Idle ───────────────────────────────────────────────────────────────

void netdev_rx_irq_all(bool enable)
{
    for (int i = 0; i < device_count; ++i)
        if (devices[i]->ops && devices[i]->ops->rx_irq)
            devices[i]->ops->rx_irq(devices[i], 0, enable);
}

void netdev_idle_wait(void)
{
    netdev_rx_irq_all(true);

    // A packet that arrived before the interrupt was enabled raises none
    if (netdev_poll_all() == 0)
        __asm__ volatile("sti; hlt; cli" ::: "memory");

    netdev_rx_irq_all(false);
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

