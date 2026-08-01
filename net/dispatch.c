/**
 * @file dispatch.c
 * @brief Software RSS — packet distribution across worker cores.
 *
 * The distributor peeks at Ethernet and IP headers to extract the
 * flow 5-tuple without modifying the packet. The packet is enqueued
 * on a worker's SPSC ring as a raw frame; the worker calls
 * @c eth_input() for full stack processing.
 */

#include "dispatch.h"
#include "netdev.h"
#include "ethernet.h"
#include "ip.h"
#include "endian.h"
#include "serial.h"
#include "string.h"

// ── State ──────────────────────────────────────────────────────────────

/// Per-worker SPSC rings (indexed by worker number, 0-based).
static struct spsc_ring *worker_rings[DISPATCH_MAX_WORKERS];

/// Core indices of each worker (indexed by worker number).
static int worker_core_ids[DISPATCH_MAX_WORKERS];

/// Total number of configured workers.
static int num_workers_active = 0;

/// Reverse map: core index → SPSC ring (@c NULL if not a worker).
static struct spsc_ring * core_ring_map[SMP_MAX_CORES];

/// Distributor-side statistics (single-writer, no lock needed).
static struct dispatch_stats stats;

// ── Initialization ─────────────────────────────────────────────────────

void dispatch_init(const int *worker_cores, int num_workers)
{
    if (num_workers > DISPATCH_MAX_WORKERS)
        num_workers = DISPATCH_MAX_WORKERS;

    memset(&stats, 0, sizeof(stats));
    memset(core_ring_map, 0, sizeof(core_ring_map));

    for (int i = 0; i < num_workers; ++i)
    {
        struct spsc_ring *ring = spsc_ring_create(DISPATCH_RING_SIZE);
        if (!ring)
        {
            kprintf("[DISPATCH] Failed to create ring for worker %d\n", i);
            continue;
        }

        worker_rings[i]    = ring;
        worker_core_ids[i] = worker_cores[i];
        core_ring_map[worker_cores[i]] = ring;
        num_workers_active++;
    }

    kprintf("[DISPATCH] Initialized: %d worker(s), ring size %d\n",
            num_workers_active, DISPATCH_RING_SIZE);
}

// ── Packet classification and distribution ─────────────────────────────

/**
 * @brief Distribute a single received packet.
 *
 * Peeks at the Ethernet and IP headers (without modifying the packet)
 * to determine the flow hash. Non-IP packets are processed directly
 * on the distributor core.
 *
 * @param nb  Network buffer containing the raw Ethernet frame.
 *            @c nb->dev must be set.
 */
static void dispatch_one(struct netbuf *nb)
{
    // Non-IP traffic (ARP, etc.): process on the distributor core
    if (nb->len < ETH_HLEN)
    {
        netbuf_free(nb);
        return;
    }

    struct ethhdr *eth = (struct ethhdr *)nb->data;
    uint16_t ethertype = ntohs(eth->ethertype);

    if (ethertype != ETH_P_IP)
    {
        eth_input(nb);
        stats.direct++;
        return;
    }

    // IP traffic: extract the 5-tuple for flow hashing
    if (nb->len < ETH_HLEN + IP_HDR_MIN)
    {
        // Too short for a valid IP packet - let the stack handle it
        eth_input(nb);
        stats.direct++;
        return;
    }

    struct iphdr *ip = (struct iphdr *)(nb->data + ETH_HLEN);
    uint32_t src_ip  = ip->saddr;  // network byte order
    uint32_t dst_ip  = ip->daddr;
    uint8_t  proto   = ip->protocol;

    // Extract transport ports if available
    uint16_t src_port = 0;
    uint16_t dst_port = 0;
    size_t ip_hdr_len = (size_t)ip->ihl * 4;

    if ((proto == IPPROTO_UDP || proto == IPPROTO_TCP)
     && nb->len >= ETH_HLEN + ip_hdr_len + 4)
    {
        const uint8_t *transport = nb->data + ETH_HLEN + ip_hdr_len;
        src_port = *(const uint16_t *)(transport);
        dst_port = *(const uint16_t *)(transport + 2);
    }

    // Hash the 5-tuple to a worker index
    uint32_t hash = flow_hash_5tuple(src_ip, dst_ip,
                                     src_port, dst_port, proto);
    int worker = (int)(hash % (uint32_t)num_workers_active);

    // Enqueue on the worker's ring
    if (!spsc_ring_enqueue(worker_rings[worker], nb))
    {
        // Ring full - drop the packet
        netbuf_free(nb);
        stats.drops++;
        return;
    }

    stats.distributed++;
}

// ── Batch distribution ─────────────────────────────────────────────────

void dispatch_rx(void)
{
    struct netdev *dev = netdev_get_default();
    if (!dev)
        return;

    struct netbuf *nb;
    while ((nb = netbuf_queue_pop(&dev->rx_queue)) != NULL)
        dispatch_one(nb);
}

// ── Core loops ─────────────────────────────────────────────────────────

NORETURN void dispatch_distributor_loop(int core_index)
{
    (void)core_index;

    kprintf("[DISPATCH] Distributor running on core %d\n", core_index);

    for (;;)
    {
        // Poll the NIC
        netdev_poll_all();

        // Distribute queued packets to workers
        dispatch_rx();

        // Yield the pipeline - reduces power and bus contention
        // while maintaining low latency
        __asm__ volatile("pause");
    }
}

NORETURN void dispatch_worker_loop(int core_index)
{
    struct spsc_ring *ring = core_ring_map[core_index];

    if (!ring)
    {
        kprintf("[DISPATCH] Worker core %d has no ring - idling\n",
                core_index);
        for (;;)
            __asm__ volatile("sti; hlt");

    }

    kprintf("[DISPATCH] Worker running on core %d\n", core_index);

    for (;;)
    {
        void *ptr;
        while (spsc_ring_dequeue(ring, &ptr))
        {
            kprintf("[WORKER] Successfully received packet; processing\n");
            struct netbuf *nb = (struct netbuf *)ptr;
            eth_input(nb);
        }

        __asm__ volatile("pause");
    }
}

// ── Queries ────────────────────────────────────────────────────────────

struct spsc_ring *dispatch_get_ring(int worker_index)
{
    if (worker_index < 0 || worker_index >= num_workers_active)
        return NULL;
    return worker_rings[worker_index];
}

struct spsc_ring *dispatch_get_core_ring(int core_index)
{
    if (core_index < 0 || core_index >= SMP_MAX_CORES)
        return NULL;
    return core_ring_map[core_index];
}

void dispatch_get_stats(struct dispatch_stats *stats_out)
{
    if (stats_out)
        *stats_out = stats;
}

