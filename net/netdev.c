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
#include "smp.h"

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
    // Cache-lined aligned: each queue's counters get a line of their own
    struct netdev *dev = kaligned_alloc(sizeof(struct netdev), 64);
    if (!dev)
        return NULL;
    memset(dev, 0, sizeof(*dev));

    if (name)
        strlcpy(dev->name, name, sizeof(dev->name));

    dev->mtu = ETH_MTU;
    dev->num_queues = 1;
    dev->max_queues = 1;

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

void netdev_get_stats(struct netdev *dev, struct netdev_stats *out)
{
    memset(out, 0, sizeof(*out));
    for (int q = 0; q < NETDEV_MAX_QUEUES; ++q)
    {
        const struct netdev_queue_stats *s = &dev->qstats[q];
        out->rx_packets += s->rx_packets;
        out->rx_bytes   += s->rx_bytes;
        out->tx_packets += s->tx_packets;
        out->tx_bytes   += s->tx_bytes;
        out->tx_dropped += s->tx_dropped;
    }
}

// ── Queues ─────────────────────────────────────────────────────────────

uint16_t netdev_set_queues(struct netdev *dev, uint16_t n)
{
    if (n == 0)
        n = 1;
    if (n > dev->max_queues)
        n = dev->max_queues;

    if (dev->ops && dev->ops->set_queues)
        dev->num_queues = dev->ops->set_queues(dev, n);
    else
        dev->num_queues = 1;

    return dev->num_queues;
}

// ── Data path - transmit ───────────────────────────────────────────────

/**
 * Transmissions a core holds back. Each core has its own: one core never
 * touches another's.
 */
struct tx_hold
{
    bool deferred;  ///< Inside a defer section.
    uint8_t used;   ///< Entries of @c held in use.
    struct
    {
        struct netdev *dev;
        uint16_t qid;
        uint16_t n;
        struct netbuf *pkts[NETDEV_BURST_MAX];
    } held[NETDEV_MAX];
} ALIGNED(64);

static struct tx_hold tx_holds[SMP_MAX_CORES];

uint16_t netdev_tx_burst(struct netdev *dev, uint16_t qid,
                         struct netbuf **pkts, uint16_t n)
{
    if (!dev->ops || !dev->ops->tx_burst || qid >= NETDEV_MAX_QUEUES)
        return 0;

    if (n > NETDEV_BURST_MAX)
        n = NETDEV_BURST_MAX;

    // Read the lengths first: accepted packets belong to the driver,
    // which may already have freed them when it returns
    uint32_t lens[NETDEV_BURST_MAX];
    for (uint16_t i = 0; i < n; ++i)
        lens[i] = pkts[i]->len;

    uint16_t sent = dev->ops->tx_burst(dev, qid, pkts, n);

    struct netdev_queue_stats *s = &dev->qstats[qid];
    s->tx_packets += sent;
    for (uint16_t i = 0; i < sent; ++i)
        s->tx_bytes += lens[i];

    return sent;
}

/// Post one held group; drop what the driver refuses.
static void tx_flush(struct netdev *dev, uint16_t qid,
                     struct netbuf **pkts, uint16_t held)
{
    uint16_t sent = netdev_tx_burst(dev, qid, pkts, held);

    for (uint16_t i = sent; i < held; ++i)
    {
        dev->qstats[qid].tx_dropped++;
        netbuf_free(pkts[i]);
    }
}

int netdev_send_queue(struct netdev *dev, uint16_t qid, struct netbuf *nb)
{
    if (!dev || !nb)
        return -1;

    if (!netdev_is_up(dev) || !dev->ops || !dev->ops->tx_burst
        || qid >= dev->num_queues)
    {
        dev->qstats[0].tx_dropped++;
        netbuf_free(nb);
        return -1;
    }

    struct tx_hold *h = &tx_holds[smp_core_id()];
    if (h->deferred)
    {
        // This core's group for (dev, qid); a core sends on one queue
        // per device, so a different qid just starts a new group
        int g = 0;
        while (g < h->used && !(h->held[g].dev == dev
                             && h->held[g].qid == qid))
            g++;
        if (g == h->used && h->used < NETDEV_MAX)
        {
            h->held[g].dev = dev;
            h->held[g].qid = qid;
            h->held[g].n   = 0;
            h->used++;
        }

        if (g < h->used)
        {
            if (h->held[g].n == NETDEV_BURST_MAX)
            {
                tx_flush(dev, qid, h->held[g].pkts, h->held[g].n);
                h->held[g].n = 0;
            }
            h->held[g].pkts[h->held[g].n++] = nb;
            return 0;
        }
        // No group free: send now
    }

    if (netdev_tx_burst(dev, qid, &nb, 1) == 1)
        return 0;

    dev->qstats[qid].tx_dropped++;
    netbuf_free(nb);
    return -1;
}

int netdev_send(struct netdev *dev, struct netbuf *nb)
{
    return netdev_send_queue(dev, 0, nb);
}

void netdev_tx_defer_begin(void)
{
    tx_holds[smp_core_id()].deferred = true;
}

void netdev_tx_defer_end(void)
{
    struct tx_hold *h = &tx_holds[smp_core_id()];
    h->deferred = false;

    for (int g = 0; g < h->used; ++g)
        if (h->held[g].n > 0)
            tx_flush(h->held[g].dev, h->held[g].qid,
                     h->held[g].pkts, h->held[g].n);
    h->used = 0;
}

// ── Data path - receive ────────────────────────────────────────────────

uint16_t netdev_rx_burst(struct netdev *dev, uint16_t qid,
                         struct netbuf **pkts, uint16_t max)
{
    if (!netdev_is_up(dev) || !dev->ops || !dev->ops->rx_burst
        || qid >= dev->num_queues)
        return 0;

    uint16_t n = dev->ops->rx_burst(dev, qid, pkts, max);

    struct netdev_queue_stats *s = &dev->qstats[qid];
    for (uint16_t i = 0; i < n; ++i)
    {
        pkts[i]->dev   = dev;
        pkts[i]->queue = qid;
        s->rx_bytes += pkts[i]->len;
    }
    s->rx_packets += n;

    return n;
}

// ── Polling ────────────────────────────────────────────────────────────

uint16_t netdev_poll_queue(struct netdev *dev, uint16_t qid)
{
    struct netbuf *pkts[NETDEV_BURST_MAX];
    uint16_t n = netdev_rx_burst(dev, qid, pkts, NETDEV_BURST_MAX);

    for (uint16_t i = 0; i < n; ++i)
        eth_input(pkts[i]);

    return n;
}

uint16_t netdev_poll(struct netdev *dev)
{
    return netdev_poll_queue(dev, 0);
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

