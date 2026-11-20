/**
 * @file arp.c
 * @brief ARP implementation.
 *
 * Runs on the core that owns the cache. Nothing here prints on the
 * packet path: events are counted.
 */

#include "arp.h"
#include "netdev.h"
#include "ethernet.h"
#include "ip.h"
#include "net.h"
#include "endian.h"
#include "timer.h"
#include "serial.h"
#include "string.h"

// ── ARP cache ──────────────────────────────────────────────────────────

static struct arp_entry arp_cache[ARP_CACHE_SIZE];

// ── Cache helpers ──────────────────────────────────────────────────────

/// Find the (@p dev, @p ip) entry. Returns @c NULL if there is none.
static struct arp_entry *arp_cache_find(struct netdev *dev, uint32_t ip)
{
    for (int i = 0; i < ARP_CACHE_SIZE; ++i)
    {
        struct arp_entry *e = &arp_cache[i];
        if (e->state != ARP_STATE_FREE && e->ip == ip && e->dev == dev)
            return e;
    }
    return NULL;
}

/// Free @p e, dropping (and counting) the packets queued on it.
static void arp_entry_free(struct arp_entry *e)
{
    struct netbuf *nb;
    while ((nb = netbuf_queue_pop(&e->pending)) != NULL)
    {
        net_stat_inc(NET_STAT_ARP_QUEUE_DROP);
        netbuf_free(nb);
    }
    e->state = ARP_STATE_FREE;
    e->dev   = NULL;
}

/**
 * @brief Allocate a cache entry for (@p dev, @p ip) in @p state.
 *
 * Tries free slots first, then evicts the least recently confirmed
 * non-static entry. Returns @c NULL (and counts @c arp_cache_full) only
 * if every entry is static.
 */
static struct arp_entry *arp_cache_alloc(struct netdev *dev, uint32_t ip,
                                         uint8_t state, uint64_t now)
{
    struct arp_entry *slot = NULL;

    for (int i = 0; i < ARP_CACHE_SIZE && !slot; ++i)
        if (arp_cache[i].state == ARP_STATE_FREE)
            slot = &arp_cache[i];

    if (!slot)
    {
        uint64_t oldest = ~0UL;
        for (int i = 0; i < ARP_CACHE_SIZE; ++i)
            if (arp_cache[i].state != ARP_STATE_STATIC
                && arp_cache[i].updated < oldest)
            {
                oldest = arp_cache[i].updated;
                slot   = &arp_cache[i];
            }

        if (!slot)
        {
            net_stat_inc(NET_STAT_ARP_CACHE_FULL);
            return NULL;
        }
        arp_entry_free(slot);
    }

    slot->dev      = dev;
    slot->ip       = ip;
    slot->state    = state;
    slot->probes   = 0;
    slot->updated  = now;
    slot->deadline = 0;
    memset(slot->mac, 0, ETH_ALEN);
    netbuf_queue_init(&slot->pending);
    return slot;
}

/// Check if @p ip is a broadcast address on @p dev's subnet.
static bool is_broadcast_ip(struct netdev *dev, uint32_t ip)
{
    if (ip == 0xFFFFFFFF)
        return true;
    if (dev && dev->netmask != 0)
    {
        uint32_t broadcast = (dev->ip_addr & dev->netmask) | ~dev->netmask;
        return ip == broadcast;
    }
    return false;
}

/// Record @p mac for @p e as just confirmed, and send what waited for it.
static void arp_entry_confirm(struct arp_entry *e, const uint8_t *mac,
                              uint64_t now)
{
    eth_copy_addr(e->mac, mac);
    e->state   = ARP_STATE_REACHABLE;
    e->probes  = 0;
    e->updated = now;

    struct netbuf *pending;
    while ((pending = netbuf_queue_pop(&e->pending)) != NULL)
        ip_output_resolved(pending, e->mac);
}

// ── Initialization ─────────────────────────────────────────────────────

void arp_init(void)
{
    memset(arp_cache, 0, sizeof(arp_cache));

    for (int i = 0; i < ARP_CACHE_SIZE; ++i)
        netbuf_queue_init(&arp_cache[i].pending);

    kprintf("[ARP] Initialized (cache: %d entries, reachable %d s, "
            "%d requests %d ms apart)\n", ARP_CACHE_SIZE,
            ARP_REACHABLE_MS / 1000, ARP_MAX_PROBES, ARP_RETRANS_MS);
}

// ── ARP packet construction ────────────────────────────────────────────

/**
 * @brief Build and send one ARP packet from @p dev.
 *
 * @param op          @c ARP_OP_REQUEST or @c ARP_OP_REPLY.
 * @param target_ip   Target protocol address.
 * @param target_mac  Target hardware address (zero for a request).
 * @param eth_dest    Ethernet destination (broadcast, or unicast for a
 *                    reply or a refresh).
 */
static void arp_send(struct netdev *dev, uint16_t op, uint32_t target_ip,
                     const uint8_t *target_mac, const uint8_t *eth_dest)
{
    struct netbuf *nb = netbuf_alloc(0);
    if (!nb)
        return;

    nb->dev = dev;
    netbuf_reserve(nb, ETH_HLEN);

    struct arphdr *arp = (struct arphdr *)netbuf_put(nb, ARP_HDR_SIZE);
    if (!arp)
    {
        netbuf_free(nb);
        return;
    }

    arp->hw_type    = htons(ARP_HW_ETHER);
    arp->proto_type = htons(ETH_P_IP);
    arp->hw_len     = ETH_ALEN;
    arp->proto_len  = 4;
    arp->operation  = htons(op);

    netdev_get_mac(dev, arp->sender_mac);
    arp->sender_ip = htonl(dev->ip_addr);

    eth_copy_addr(arp->target_mac, target_mac);
    arp->target_ip = htonl(target_ip);

    net_stat_inc(op == ARP_OP_REQUEST ? NET_STAT_ARP_TX_REQUEST
                                      : NET_STAT_ARP_TX_REPLY);

    // Consumes nb
    eth_output(nb, eth_dest, ETH_P_ARP);
}

void arp_send_request(struct netdev *dev, uint32_t target_ip)
{
    arp_send(dev, ARP_OP_REQUEST, target_ip, ETH_ZERO, ETH_BROADCAST);
}

/// Send the next request of a resolution or refresh and arm its timer.
/// A refresh asks the known MAC directly (RFC 1122 §2.3.2.1), except for
/// its last try, which is broadcast in case the neighbor's MAC changed.
static void arp_probe(struct arp_entry *e, uint64_t now)
{
    bool unicast = e->state == ARP_STATE_STALE
                && e->probes + 1 < ARP_MAX_PROBES;

    arp_send(e->dev, ARP_OP_REQUEST, e->ip, ETH_ZERO,
             unicast ? e->mac : ETH_BROADCAST);

    if (e->probes > 0)
        net_stat_inc(NET_STAT_ARP_RETRANSMIT);
    e->probes++;
    e->deadline = now + ARP_RETRANS_MS;
}

// ── RX path ────────────────────────────────────────────────────────────

void arp_input(struct netbuf *nb)
{
    if (!nb)
        return;

    struct netdev *dev = nb->dev;
    struct arphdr *arp = (struct arphdr *)nb->data;

    // Validate: Ethernet + IPv4, a known operation
    uint16_t operation = nb->len >= ARP_HDR_SIZE ? ntohs(arp->operation) : 0;
    if (!dev || nb->len < ARP_HDR_SIZE
        || ntohs(arp->hw_type)    != ARP_HW_ETHER
        || ntohs(arp->proto_type) != ETH_P_IP
        || arp->hw_len            != ETH_ALEN
        || arp->proto_len         != 4
        || (operation != ARP_OP_REQUEST && operation != ARP_OP_REPLY))
    {
        net_stat_inc(NET_STAT_ARP_MALFORMED);
        netbuf_free(nb);
        return;
    }

    net_stat_inc(operation == ARP_OP_REQUEST ? NET_STAT_ARP_RX_REQUEST
                                             : NET_STAT_ARP_RX_REPLY);

    uint32_t sender_ip = ntohl(arp->sender_ip);
    uint32_t target_ip = ntohl(arp->target_ip);
    uint64_t now       = timer_get_ms();

    // A sender MAC that is not a station's is never learned
    if (eth_is_multicast(arp->sender_mac) || eth_is_zero(arp->sender_mac))
    {
        net_stat_inc(NET_STAT_ARP_IGNORED);
        netbuf_free(nb);
        return;
    }

    // Another station claiming our address: never leanred
    if (dev->ip_addr != 0 && sender_ip == dev->ip_addr)
    {
        net_stat_inc(NET_STAT_ARP_CONFLICT);
        netbuf_free(nb);
        return;
    }

    // A sender off this interface's subnet is not our neighbor here.
    // Sender 0.0.0.0 is an address probe (RFC 5227): answered if it asks
    // for our address, never learned.
    bool learnable = sender_ip != 0;
    if (learnable && dev->netmask != 0
        && (sender_ip & dev->netmask) != (dev->ip_addr & dev->netmask))
    {
        net_stat_inc(NET_STAT_ARP_IGNORED);
        netbuf_free(nb);
        return;
    }

    // RFC 826: if we already have an entry for the sender on this
    // interface, refresh it whether or not the packet targets us.
    // Configured (static) entries are never overwritten.
    struct arp_entry *entry = learnable ? arp_cache_find(dev, sender_ip) : NULL;
    if (entry && entry->state != ARP_STATE_STATIC)
        arp_entry_confirm(entry, arp->sender_mac, now);

    // The rest only concerns packets for our address
    if (dev->ip_addr == 0 || target_ip != dev->ip_addr)
    {
        netbuf_free(nb);
        return;
    }

    // RFC 826: targeted at us - record the sender if it is new
    if (learnable && !entry)
    {
        entry = arp_cache_alloc(dev, sender_ip, ARP_STATE_REACHABLE, now);
        if (entry)
            eth_copy_addr(entry->mac, arp->sender_mac);
    }

    if (operation == ARP_OP_REQUEST)
        arp_send(dev, ARP_OP_REPLY, sender_ip, arp->sender_mac,
                 arp->sender_mac);

    netbuf_free(nb);
}

// ── Address resolution ─────────────────────────────────────────────────

int arp_resolve(struct netdev *dev, uint32_t ip, uint8_t *mac)
{
    if (!dev || !mac)
        return ARP_ERROR;

    // Broadcast addresses resolve to the broadcast MAC
    if (is_broadcast_ip(dev, ip))
    {
        eth_copy_addr(mac, ETH_BROADCAST);
        return ARP_OK;
    }

    struct arp_entry *entry = arp_cache_find(dev, ip);
    if (entry)
    {
        // Reachable, stale (being refreshed) or static: the MAC is good.
        // A lookup never changes an entry: aging is arp_timer()'s job.
        if (entry->state != ARP_STATE_INCOMPLETE)
        {
            eth_copy_addr(mac, entry->mac);
            return ARP_OK;
        }
        return ARP_PENDING;
    }

    // Not in cache: start resolution
    uint64_t now = timer_get_ms();
    entry = arp_cache_alloc(dev, ip, ARP_STATE_INCOMPLETE, now);
    if (!entry)
        return ARP_ERROR;

    arp_probe(entry, now);
    return ARP_PENDING;
}

int arp_queue_packet(struct netdev *dev, uint32_t ip, struct netbuf *nb)
{
    if (!dev || !nb)
    {
        if (nb)
            netbuf_free(nb);
        return -1;
    }

    uint8_t mac[ETH_ALEN];
    int ret = arp_resolve(dev, ip, mac);
    if (ret == ARP_OK)
        return ip_output_resolved(nb, mac);

    struct arp_entry *entry = ret == ARP_PENDING ? arp_cache_find(dev, ip) : NULL;
    if (!entry)
    {
        net_stat_inc(NET_STAT_ARP_QUEUE_DROP);
        netbuf_free(nb);
        return -1;
    }

    // Enforce the pending queue limit: drop the oldest if full
    while (entry->pending.count >= ARP_PENDING_MAX)
    {
        struct netbuf *old = netbuf_queue_pop(&entry->pending);
        if (!old)
            break;
        net_stat_inc(NET_STAT_ARP_QUEUE_DROP);
        netbuf_free(old);
    }

    netbuf_queue_push(&entry->pending, nb);
    return 0;
}

// ── Timers ─────────────────────────────────────────────────────────────

void arp_timer(uint64_t now)
{
    for (int i = 0; i < ARP_CACHE_SIZE; ++i)
    {
        struct arp_entry *e = &arp_cache[i];

        switch (e->state)
        {
            case ARP_STATE_REACHABLE:
                // Lifetime over: keep using the MAC, ask again
                if (now - e->updated >= ARP_REACHABLE_MS)
                {
                    e->state  = ARP_STATE_STALE;
                    e->probes = 0;
                    arp_probe(e, now);
                }
                break;

            case ARP_STATE_INCOMPLETE:
            case ARP_STATE_STALE:
                if (now < e->deadline)
                    break;
                if (e->probes < ARP_MAX_PROBES)
                    arp_probe(e, now);
                else
                {
                    net_stat_inc(e->state == ARP_STATE_INCOMPLETE
                                 ? NET_STAT_ARP_RESOLVE_FAILED
                                 : NET_STAT_ARP_EXPIRED);
                    arp_entry_free(e);
                }
                break;

            default:
                break;
        }
    }
}

bool arp_timers_pending(void)
{
    for (int i = 0; i < ARP_CACHE_SIZE; ++i)
        if (arp_cache[i].state == ARP_STATE_INCOMPLETE)
            return true;
    return false;
}

// ── Cache management ───────────────────────────────────────────────────

void arp_add_static(uint32_t ip, const uint8_t *mac, struct netdev *dev)
{
    uint64_t now = timer_get_ms();
    struct arp_entry *entry = arp_cache_find(dev, ip);

    if (!entry)
        entry = arp_cache_alloc(dev, ip, ARP_STATE_STATIC, now);
    if (!entry)
        return;

    // An entry that was waiting gets its packets sent
    arp_entry_confirm(entry, mac, now);
    entry->state = ARP_STATE_STATIC;
}

int arp_lookup(struct netdev *dev, uint32_t ip, uint8_t *mac)
{
    struct arp_entry *entry = arp_cache_find(dev, ip);
    if (entry && entry->state != ARP_STATE_INCOMPLETE)
    {
        eth_copy_addr(mac, entry->mac);
        return 0;
    }
    return -1;
}

int arp_entry_state(struct netdev *dev, uint32_t ip)
{
    struct arp_entry *entry = arp_cache_find(dev, ip);
    return entry ? entry->state : ARP_STATE_FREE;
}

void arp_flush_dev(struct netdev *dev)
{
    for (int i = 0; i < ARP_CACHE_SIZE; ++i)
        if (arp_cache[i].state != ARP_STATE_FREE && arp_cache[i].dev == dev)
            arp_entry_free(&arp_cache[i]);
}

// ── Debug ──────────────────────────────────────────────────────────────

void arp_cache_dump(void)
{
    kprintf("[ARP] Cache contents:\n");

    static const char *state_names[] =
    {
        [ARP_STATE_FREE]       = "free",
        [ARP_STATE_INCOMPLETE] = "incomplete",
        [ARP_STATE_REACHABLE]  = "reachable",
        [ARP_STATE_STALE]      = "stale",
        [ARP_STATE_STATIC]     = "static",
    };

    uint64_t now = timer_get_ms();
    for (int i = 0; i < ARP_CACHE_SIZE; ++i)
    {
        struct arp_entry *e = &arp_cache[i];
        if (e->state == ARP_STATE_FREE)
            continue;

        kprintf("  %s %s -> %s  [%s, age=%lu ms, probes=%u, pending=%lu]\n",
                e->dev ? e->dev->name : "?", IP4_FMT(e->ip), ETH_FMT(e->mac),
                state_names[e->state], now - e->updated, e->probes,
                (uint64_t)e->pending.count);
    }
}

