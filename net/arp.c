/**
 * @file arp.c
 * @brief ARP implementation.
 */

#include "arp.h"
#include "netdev.h"
#include "ethernet.h"
#include "ip.h"
#include "endian.h"
#include "timer.h"
#include "serial.h"
#include "string.h"

// ── ARP cache ──────────────────────────────────────────────────────────

static struct arp_entry arp_cache[ARP_CACHE_SIZE];

// ── Cache helpers ──────────────────────────────────────────────────────

/// Find an existing cache entry by IP address. Returns @c NULL if not found.
static struct arp_entry *arp_cache_find(uint32_t ip)
{
    for (int i = 0; i < ARP_CACHE_SIZE; ++i)
        if (arp_cache[i].state != ARP_STATE_FREE && arp_cache[i].ip == ip)
            return &arp_cache[i];
    return NULL;
}

/**
 * @brief Allocate a cache entry.
 *
 * Tries free slots first, then evicts the oldest non-static entry.
 * Purges any pending packets on eviction.
 */
static struct arp_entry *arp_cache_alloc(void)
{
    // Look for a free slot
    for (int i = 0; i < ARP_CACHE_SIZE; ++i)
        if (arp_cache[i].state == ARP_STATE_FREE)
            return &arp_cache[i];

    // Evict the oldest non-static entry
    uint64_t oldest_time = ~0ULL;
    struct arp_entry *oldest = NULL;

    for (int i = 0; i < ARP_CACHE_SIZE; ++i)
        if (arp_cache[i].state != ARP_STATE_STATIC &&
            arp_cache[i].timestamp < oldest_time)
        {
            oldest_time = arp_cache[i].timestamp;
            oldest = &arp_cache[i];
        }

    if (oldest)
    {
        netbuf_queue_purge(&oldest->pending);
        oldest->state = ARP_STATE_FREE;
    }

    return oldest;
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

// ── Initialization ─────────────────────────────────────────────────────

void arp_init(void)
{
    memset(arp_cache, 0, sizeof(arp_cache));

    for (int i = 0; i < ARP_CACHE_SIZE; ++i)
        netbuf_queue_init(&arp_cache[i].pending);

    kprintf("[ARP] Initialized (cache: %d entries, timeout: %ds)\n",
            ARP_CACHE_SIZE, ARP_CACHE_TIMEOUT);
}

// ── ARP packet construction ────────────────────────────────────────────

void arp_send_request(struct netdev *dev, uint32_t target_ip)
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
    arp->operation  = htons(ARP_OP_REQUEST);

    netdev_get_mac(dev, arp->sender_mac);
    arp->sender_ip = htonl(dev->ip_addr);

    memset(arp->target_mac, 0, ETH_ALEN);
    arp->target_ip = htonl(target_ip);

    // Consumes nb
    eth_output(nb, ETH_BROADCAST, ETH_P_ARP);

    kprintf("[ARP] Sent request: who has %s?\n", IP4_FMT(target_ip));
}

/// Send a unicast ARP reply.
static void arp_send_reply(struct netdev *dev, uint32_t target_ip,
                           const uint8_t *target_mac)
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
    arp->operation  = htons(ARP_OP_REPLY);

    netdev_get_mac(dev, arp->sender_mac);
    arp->sender_ip = htonl(dev->ip_addr);

    eth_copy_addr(arp->target_mac, target_mac);
    arp->target_ip = htonl(target_ip);

    // Consumes nb
    eth_output(nb, target_mac, ETH_P_ARP);
}

// ── RX path ────────────────────────────────────────────────────────────

void arp_input(struct netbuf *nb)
{
    if (!nb)
        return;

    if (nb->len < ARP_HDR_SIZE)
    {
        netbuf_free(nb);
        return;
    }

    struct arphdr *arp = (struct arphdr *)nb->data;

    // Validate: must be Ethernet + IPv4
    if (ntohs(arp->hw_type)    != ARP_HW_ETHER ||
        ntohs(arp->proto_type) != ETH_P_IP     ||
        arp->hw_len            != ETH_ALEN     ||
        arp->proto_len         != 4)
    {
        netbuf_free(nb);
        return;
    }

    uint32_t sender_ip = ntohl(arp->sender_ip);
    uint32_t target_ip = ntohl(arp->target_ip);
    uint16_t operation = ntohs(arp->operation);

    // RFC 826: If there already exists an entry for the sender, update it
    // regardless of whether the packet targets us. This keeps the cache
    // fresh from any ARP traffic observed.
    struct arp_entry *entry = arp_cache_find(sender_ip);
    if (entry)
    {
        eth_copy_addr(entry->mac, arp->sender_mac);
        if (entry->state == ARP_STATE_PENDING
            || entry->state == ARP_STATE_RESOLVED)
            entry->state = ARP_STATE_RESOLVED;
        entry->timestamp = timer_get_ticks();

        // Flush packets that were waiting for this resolution
        struct netbuf *pending;
        while ((pending = netbuf_queue_pop(&entry->pending)) != NULL)
            ip_output_resolved(pending, entry->mac);
    }

    // Is this ARP directed at our IP?
    struct netdev *dev = nb->dev;
    if (!dev || dev->ip_addr == 0 || target_ip != dev->ip_addr)
    {
        netbuf_free(nb);
        return;
    }

    if (operation == ARP_OP_REQUEST)
    {
        char sender_str[16];
        char target_str[16];

        kprintf("[ARP] Request from %s for %s\n",
                ip4_addr_to_str(sender_ip, sender_str),
                ip4_addr_to_str(target_ip, target_str));

        arp_send_reply(dev, sender_ip, arp->sender_mac);

        // Cache the requester's address if we don't have it yet
        if (!entry)
        {
            entry = arp_cache_alloc();
            if (entry)
            {
                entry->ip        = sender_ip;
                eth_copy_addr(entry->mac, arp->sender_mac);
                entry->state     = ARP_STATE_RESOLVED;
                entry->timestamp = timer_get_ticks();
                entry->dev       = dev;
                netbuf_queue_init(&entry->pending);
            }
        }
    }
    else if (operation == ARP_OP_REPLY)
    {
        kprintf("[ARP] Reply: %s is %s\n",
                IP4_FMT(sender_ip), ETH_FMT(arp->sender_mac));

        // Create an entry if one doesn't already exist
        if (!entry)
        {
            entry = arp_cache_alloc();
            if (entry)
            {
                entry->ip        = sender_ip;
                eth_copy_addr(entry->mac, arp->sender_mac);
                entry->state     = ARP_STATE_RESOLVED;
                entry->timestamp = timer_get_ticks();
                entry->dev       = dev;
                netbuf_queue_init(&entry->pending);
            }
        }
    }

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

    struct arp_entry *entry = arp_cache_find(ip);

    if (entry)
    {
        if (entry->state == ARP_STATE_RESOLVED
            || entry->state == ARP_STATE_STATIC)
        {
            // Check for expiry (static entries never expire)
            if (entry->state != ARP_STATE_STATIC)
            {
                uint64_t age =
                    (timer_get_ticks() - entry->timestamp) / TIMER_HZ;
                if (age > ARP_CACHE_TIMEOUT)
                {
                    // Entry expired — re-resolve
                    entry->state     = ARP_STATE_PENDING;
                    entry->timestamp = timer_get_ticks();
                    arp_send_request(dev, ip);
                    return ARP_PENDING;
                }
            }
            eth_copy_addr(mac, entry->mac);
            return ARP_OK;
        }

        if (entry->state == ARP_STATE_PENDING)
            return ARP_PENDING;
    }

    // Not in cache - create a pending entry and send a request
    entry = arp_cache_alloc();
    if (!entry)
        return ARP_ERROR;

    entry->ip        = ip;
    memset(entry->mac, 0, ETH_ALEN);
    entry->state     = ARP_STATE_PENDING;
    entry->timestamp = timer_get_ticks();
    entry->dev       = dev;
    netbuf_queue_init(&entry->pending);

    arp_send_request(dev, ip);
    return ARP_PENDING;
}

int arp_queue_packet(struct netdev *dev, uint32_t ip, struct netbuf *nb)
{
    if (!dev || !nb)
        return -1;

    struct arp_entry *entry = arp_cache_find(ip);

    if (!entry)
    {
        // Start resolution if not already underway
        uint8_t mac[ETH_ALEN];
        int ret = arp_resolve(dev, ip, mac);
        if (ret == ARP_OK)
            return ip_output_resolved(nb, mac);
        else if (ret == ARP_ERROR)
        {
            netbuf_free(nb);
            return -1;
        }
        entry = arp_cache_find(ip);
    }

    if (!entry)
    {
        netbuf_free(nb);
        return -1;
    }

    // Enforce the pending queue limit - drop the oldest if full
    while (entry->pending.count >= ARP_PENDING_MAX)
    {
        struct netbuf *old = netbuf_queue_pop(&entry->pending);
        if (old)
            netbuf_free(old);
    }

    netbuf_queue_push(&entry->pending, nb);
    return 0;
}

// ── Cache management ───────────────────────────────────────────────────

void arp_add_static(uint32_t ip, const uint8_t *mac, struct netdev *dev)
{
    struct arp_entry *entry = arp_cache_find(ip);
    if (!entry)
        entry = arp_cache_alloc();
    if (!entry)
        return;

    entry->ip        = ip;
    eth_copy_addr(entry->mac, mac);
    entry->state     = ARP_STATE_STATIC;
    entry->timestamp = timer_get_ticks();
    entry->dev       = dev;
    netbuf_queue_init(&entry->pending);

    kprintf("[ARP] Static entry: %s -> %s\n", IP4_FMT(ip), ETH_FMT(mac));
}

int arp_lookup(uint32_t ip, uint8_t *mac)
{
    struct arp_entry *entry = arp_cache_find(ip);
    if (entry && (entry->state == ARP_STATE_RESOLVED ||
                  entry->state == ARP_STATE_STATIC))
    {
        eth_copy_addr(mac, entry->mac);
        return 0;
    }
    return -1;
}

void arp_cache_expire(void)
{
    uint64_t now = timer_get_ticks();

    for (int i = 0; i < ARP_CACHE_SIZE; ++i)
    {
        struct arp_entry *e = &arp_cache[i];

        if (e->state == ARP_STATE_FREE || e->state == ARP_STATE_STATIC)
            continue;

        uint64_t age = (now - e->timestamp) / TIMER_HZ;
        if (age > ARP_CACHE_TIMEOUT)
        {
            if (e->state == ARP_STATE_PENDING)
                netbuf_queue_purge(&e->pending);
            e->state = ARP_STATE_FREE;
        }
    }
}

// ── Debug ──────────────────────────────────────────────────────────────

void arp_cache_dump(void)
{
    kprintf("[ARP] Cache contents:\n");

    static const char *state_names[] =
    {
        [ARP_STATE_FREE]     = "free",
        [ARP_STATE_PENDING]  = "pending",
        [ARP_STATE_RESOLVED] = "resolved",
        [ARP_STATE_STATIC]   = "static",
    };

    for (int i = 0; i < ARP_CACHE_SIZE; ++i)
    {
        struct arp_entry *e = &arp_cache[i];
        if (e->state == ARP_STATE_FREE)
            continue;

        uint64_t age = (timer_get_ticks() - e->timestamp) / TIMER_HZ;
        kprintf("  %s -> %s  [%s, age=%lus, pending=%lu]\n",
                IP4_FMT(e->ip), ETH_FMT(e->mac),
                state_names[e->state], age,
                (uint64_t)e->pending.count);
    }
}

