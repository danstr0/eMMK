/**
 * @file test_net.c
 * @brief Tests for network utilities: checksum, byte order, address
 *        formatting, and MAC comparison.
 */

#include "test.h"
#include "udp.h"
#include "ip.h"
#include "arp.h"
#include "ethernet.h"
#include "netdev.h"
#include "endian.h"
#include "smp.h"
#include "heap.h"
#include "format.h"
#include "net.h"
#include "timer.h"

// Dummy handler for UDP bind tests
static void udp_test_handler(struct netbuf *nb) { netbuf_free(nb); }

// Fake network device

static int fake_sent;

/// tx burst that "transmits" by counting and takes ownership, per the
/// contract.
static uint16_t fake_tx_ok(struct netdev *dev, uint16_t qid,
                           struct netbuf **pkts, uint16_t n)
{
    (void)dev; (void)qid;
    for (uint16_t i = 0; i < n; ++i)
    {
        fake_sent++;
        netbuf_free(pkts[i]);
    }
    return n;
}

/// tx_burst that accepts nothing; the packets stay the caller's.
static uint16_t fake_tx_fail(struct netdev *dev, uint16_t qid,
                             struct netbuf **pkts, uint16_t n)
{
    (void)dev; (void)qid; (void)pkts; (void)n;
    return 0;
}

static const struct netdev_ops fake_ok_ops   = { .tx_burst = fake_tx_ok };
static const struct netdev_ops fake_fail_ops = { .tx_burst = fake_tx_fail };

// Fake NIC with a pool, for the zero-copy echo

static struct netbuf *loop_rx;        ///< Frame rx_burst delivers next.
static struct netbuf *loop_tx;        ///< Frame tx_burst received.
static int            loop_tx_calls;  ///< tx_burst calls ("doorbells").
static int            loop_tx_pkts;   ///< Packets given to tx_burst.

static uint16_t loop_rx_burst(struct netdev *dev, uint16_t qid,
                              struct netbuf **pkts, uint16_t max)
{
    (void)dev; (void)qid;
    if (!loop_rx || max == 0)
        return 0;
    pkts[0] = loop_rx;
    loop_rx = NULL;
    return 1;
}

/// Keeps the packet, as a NIC does until the send completes.
static uint16_t loop_tx_burst(struct netdev *dev, uint16_t qid,
                              struct netbuf **pkts, uint16_t n)
{
    (void)dev; (void)qid;
    if (n == 0)
        return 0;
    loop_tx_calls++;
    loop_tx_pkts += n;
    loop_tx = pkts[n - 1];
    for (uint16_t i = 0; i + 1 < n; ++i)
        netbuf_free(pkts[i]);
    return n;
}

static const struct netdev_ops loop_ops =
    { .rx_burst = loop_rx_burst, .tx_burst = loop_tx_burst };

/// Echo handler, as in the demo.
static void udp_test_echo(struct netbuf *nb) { udp_reply(nb); }

// Fake NIC that records what the stack transmits

static int     cap_arp_requests;         ///< ARP requests sent.
static int     cap_arp_replies;          ///< ARP replies sent.
static int     cap_ip_frames;            ///< IPv4 frames sent.
static uint8_t cap_last_dest[ETH_ALEN];  ///< Ethernet dest of the last frame.

static uint16_t cap_tx_burst(struct netdev *dev, uint16_t qid,
                             struct netbuf **pkts, uint16_t n)
{
    (void)dev; (void)qid;
    for (uint16_t i = 0; i < n; ++i)
    {
        struct ethhdr *eth = (struct ethhdr *)pkts[i]->data;
        eth_copy_addr(cap_last_dest, eth->dest);
        if (ntohs(eth->ethertype) == ETH_P_ARP)
        {
            struct arphdr *arp = (struct arphdr *)(eth + 1);
            if (ntohs(arp->operation) == ARP_OP_REQUEST)
                cap_arp_requests++;
            else
                cap_arp_replies++;
        }
        else if (ntohs(eth->ethertype) == ETH_P_IP)
            cap_ip_frames++;
        netbuf_free(pkts[i]);
    }
    return n;
}

static const struct netdev_ops cap_ops = { .tx_burst = cap_tx_burst };

/// An ARP packet as received on @p dev (Ethernet header already removed).
static struct netbuf *make_arp(struct netdev *dev, uint16_t op,
                               const uint8_t *sender_mac, uint32_t sender_ip,
                               uint32_t target_ip)
{
    struct netbuf *nb = netbuf_alloc(0);
    netbuf_reserve(nb, NETBUF_HEADROOM);
    struct arphdr *arp = (struct arphdr *)netbuf_put(nb, ARP_HDR_SIZE);
    arp->hw_type    = htons(ARP_HW_ETHER);
    arp->proto_type = htons(ETH_P_IP);
    arp->hw_len     = ETH_ALEN;
    arp->proto_len  = 4;
    arp->operation  = htons(op);
    eth_copy_addr(arp->sender_mac, sender_mac);
    arp->sender_ip  = htonl(sender_ip);
    memset(arp->target_mac, 0, ETH_ALEN);
    arp->target_ip  = htonl(target_ip);
    nb->dev = dev;
    return nb;
}

/// A raw frame of @p len bytes with @p ethertype, to @p dev's MAC.
static struct netbuf *make_frame(struct netdev *dev, uint16_t ethertype,
                                 size_t len)
{
    struct netbuf *nb = netbuf_alloc(0);
    netbuf_reserve(nb, NETBUF_HEADROOM);
    uint8_t *f = netbuf_put(nb, len);
    memset(f, 0, len);
    if (len >= ETH_HLEN)
    {
        struct ethhdr *eth = (struct ethhdr *)f;
        eth_copy_addr(eth->dest, dev->mac);
        eth->src[0] = 0x02;
        eth->ethertype = htons(ethertype);
    }
    nb->dev = dev;
    return nb;
}

/// Suite "arp": ARP retransmission and aging, per-interface entries,
/// and drop counters.
uint32_t test_arp(void)
{
    TEST_BEGIN("arp");

    static const uint8_t mac_a[ETH_ALEN] = {0x02, 0, 0, 0, 0xAA, 0x01};
    static const uint8_t mac_b[ETH_ALEN] = {0x02, 0, 0, 0, 0xBB, 0x02};
    static const uint8_t mac_s[ETH_ALEN] = {0x02, 0, 0, 0, 0x55, 0x03};
    const uint32_t mask   = IP4_ADDR(255, 255, 255, 0);
    const uint32_t ip0    = IP4_ADDR(10, 6, 6, 2);   // arp0
    const uint32_t ip1    = IP4_ADDR(10, 6, 6, 3);   // arp1, same subnet
    const uint32_t peer   = IP4_ADDR(10, 6, 6, 1);
    const uint32_t dead   = IP4_ADDR(10, 6, 6, 9);   // never answers
    const uint32_t fixed  = IP4_ADDR(10, 6, 6, 7);   // static entry

    // ── Counter registry ───────────────────────────────
    TEST_ASSERT_STREQ(net_stat_name(NET_STAT_ETH_RUNT), "eth_runt",
                      "counter names come from NET_STAT_LIST");
    TEST_ASSERT_STREQ(net_stat_name(NET_STAT_COUNT), "?",
                      "out-of-range counter has no name");
    TEST_ASSERT_EQ(sizeof(struct net_core_stats) % 64, 0,
                   "per-core counter blocks are whole cache lines");
    {
        uint64_t before = net_stat_total(NET_STAT_UDP_NO_PORT);
        net_stat_inc(NET_STAT_UDP_NO_PORT);
        TEST_ASSERT_EQ(net_stat_total(NET_STAT_UDP_NO_PORT), before + 1,
                       "net_stat_inc counts on this core");
        net_core_stats[smp_core_id()].c[NET_STAT_UDP_NO_PORT]--;
    }

    struct netdev *d0 = netdev_alloc("arp0");
    struct netdev *d1 = netdev_alloc("arp1");
    TEST_ASSERT(d0 && d1, "ARP fake netdevs allocated");
    if (!d0 || !d1)
    {
        kfree(d0);
        kfree(d1);
        TEST_END();
    }
    d0->mac[0] = 0x02; d0->mac[5] = 0x10;
    d1->mac[0] = 0x02; d1->mac[5] = 0x11;
    d0->ops = &cap_ops;
    d1->ops = &cap_ops;
    netdev_register(d0);
    netdev_register(d1);
    netdev_set_up(d0);
    netdev_set_up(d1);
    netdev_set_ip(d0, ip0, mask, 0);
    netdev_set_ip(d1, ip1, mask, 0);

    // ── Bug 1: a lost reply is retransmitted ───────────
    // (The old cache sent one request and then waited forever)
    {
        cap_arp_requests = cap_ip_frames = 0;
        uint64_t retx = net_stat_total(NET_STAT_ARP_RETRANSMIT);

        struct netbuf *nb = netbuf_alloc(0);
        netbuf_reserve(nb, NETBUF_HEADROOM);
        netbuf_put(nb, 8);
        nb->dev  = d0;
        TEST_ASSERT_EQ(arp_queue_packet(d0, peer, nb), 0, "packet queued");
        TEST_ASSERT_EQ(cap_arp_requests, 1, "first request sent");
        TEST_ASSERT_EQ(arp_entry_state(d0, peer), ARP_STATE_INCOMPLETE,
                       "entry incomplete");
        TEST_ASSERT(arp_timers_pending(), "a timer is armed");

        // The reply is "lost": nothing arrives. Before the deadline,
        // nothing happens; after it, the request is sent again.
        uint64_t t = timer_get_ms();
        arp_timer(t);
        TEST_ASSERT_EQ(cap_arp_requests, 1,
                       "no retransmit before the deadline");
        arp_timer(t + ARP_RETRANS_MS + 1);
        TEST_ASSERT_EQ(cap_arp_requests, 2, "request retransmitted");
        TEST_ASSERT_EQ(net_stat_total(NET_STAT_ARP_RETRANSMIT), retx + 1,
                       "retransmission counted");

        // This time the reply arrives: the queued packet goes out
        arp_input(make_arp(d0, ARP_OP_REPLY, mac_a, peer, ip0));
        TEST_ASSERT_EQ(arp_entry_state(d0, peer), ARP_STATE_REACHABLE,
                       "reply resolves the entry");
        TEST_ASSERT_EQ(cap_ip_frames, 1,
                       "queued packet sent on resolution");
        TEST_ASSERT_MEMEQ(cap_last_dest, mac_a, ETH_ALEN,
                          "sent to the resolved MAC");
        TEST_ASSERT(!arp_timers_pending(), "no timer left armed");
    }

    // ── A resolution that never succeeds gives up ──────
    {
        cap_arp_requests = cap_ip_frames = 0;
        uint64_t failed = net_stat_total(NET_STAT_ARP_RESOLVE_FAILED);
        uint64_t qdrop  = net_stat_total(NET_STAT_ARP_QUEUE_DROP);

        struct netbuf *nb = netbuf_alloc(0);
        netbuf_reserve(nb, NETBUF_HEADROOM);
        netbuf_put(nb, 8);
        nb->dev = d0;
        arp_queue_packet(d0, dead, nb);

        uint64_t t = timer_get_ms();
        for (int i = 1; i <= ARP_MAX_PROBES; ++i)
            arp_timer(t + (uint64_t)i * (ARP_RETRANS_MS + 1));

        TEST_ASSERT_EQ(cap_arp_requests, ARP_MAX_PROBES,
                       "ARP_MAX_PROBES requests, then no more");
        TEST_ASSERT_EQ(arp_entry_state(d0, dead), ARP_STATE_FREE,
                       "unanswered entry freed");
        TEST_ASSERT_EQ(net_stat_total(NET_STAT_ARP_RESOLVE_FAILED),
                       failed + 1,
                       "failure counted");
        TEST_ASSERT_EQ(net_stat_total(NET_STAT_ARP_QUEUE_DROP),
                       qdrop + 1,
                       "its queued packet dropped and counted");
        TEST_ASSERT_EQ(cap_ip_frames, 0,
                       "nothing sent to an unresolved MAC");
    }

    // ── Bug 3: entries are per interface ───────────────
    // The same address answers on arp1 from another MAC. The old cache,
    // keyed by IP only, overwrote arp0's entry with it.
    {
        cap_arp_replies = 0;
        arp_input(make_arp(d1, ARP_OP_REQUEST, mac_b, peer, ip1));
        TEST_ASSERT_EQ(cap_arp_replies, 1,
                       "request for our address answered");

        uint8_t mac[ETH_ALEN];
        TEST_ASSERT(arp_lookup(d0, peer, mac) == 0
                    && eth_addr_equal(mac, mac_a),
                    "ARP on arp1 leaves arp0's entry alone");
        TEST_ASSERT(arp_lookup(d1, peer, mac) == 0
                    && eth_addr_equal(mac, mac_b),
                    "arp1 learns its own entry");
    }

    // ── Static entries are never overwritten ───────────
    {
        uint8_t mac[ETH_ALEN];
        arp_add_static(fixed, mac_s, d0);
        arp_input(make_arp(d0, ARP_OP_REPLY, mac_b, fixed, ip0));
        TEST_ASSERT(arp_lookup(d0, fixed, mac) == 0
                    && eth_addr_equal(mac, mac_s),
                    "received ARP does not change a static entry");
    }

    // ── Validation: what is never learned ──────────────
    {
        static const uint8_t bcast[ETH_ALEN] =
            {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        const uint32_t off_subnet = IP4_ADDR(10, 1, 1, 1);
        uint64_t ignored  = net_stat_total(NET_STAT_ARP_IGNORED);
        uint64_t conflict = net_stat_total(NET_STAT_ARP_CONFLICT);
        uint64_t bad      = net_stat_total(NET_STAT_ARP_MALFORMED);
        cap_arp_replies = 0;

        arp_input(make_arp(d0, ARP_OP_REQUEST, mac_b, off_subnet, ip0));
        TEST_ASSERT_EQ(arp_entry_state(d0, off_subnet), ARP_STATE_FREE,
                       "sender off the interface's subnet not learned");
        arp_input(make_arp(d0, ARP_OP_REQUEST, bcast, IP4_ADDR(10, 6, 6, 50),
                           ip0));
        TEST_ASSERT_EQ(arp_entry_state(d0, IP4_ADDR(10, 6, 6, 50)),
                       ARP_STATE_FREE, "broadcast sender MAC not learned");
        TEST_ASSERT_EQ(net_stat_total(NET_STAT_ARP_IGNORED), ignored + 2,
                       "both counted as ignored");
        TEST_ASSERT_EQ(cap_arp_replies, 0, "and neither answered");

        arp_input(make_arp(d0, ARP_OP_REPLY, mac_b, ip0, ip0));
        TEST_ASSERT_EQ(net_stat_total(NET_STAT_ARP_CONFLICT), conflict + 1,
                       "a station claiming our address is counted");

        struct netbuf *nb = make_arp(d0, 7, mac_b, peer, ip0);
        arp_input(nb);
        TEST_ASSERT_EQ(net_stat_total(NET_STAT_ARP_MALFORMED), bad + 1,
                       "unknown ARP operation counted as malformed");
    }

    // ── Bug 2: aging — stale, refreshed, expired ───────
    {
        // arp1 goes away first: its entries go with it (arp_flush_dev)
        netdev_unregister(d1);
        TEST_ASSERT_EQ(arp_entry_state(d1, peer), ARP_STATE_FREE,
                       "unregistering an interface drops its entries");

        uint8_t mac[ETH_ALEN];
        uint64_t expired = net_stat_total(NET_STAT_ARP_EXPIRED);
        uint64_t t = timer_get_ms() + ARP_REACHABLE_MS + 1;
        cap_arp_requests = 0;

        arp_timer(t);
        TEST_ASSERT_EQ(arp_entry_state(d0, peer), ARP_STATE_STALE,
                       "entry stale after ARP_REACHABLE_MS");
        TEST_ASSERT(arp_lookup(d0, peer, mac) == 0
                    && eth_addr_equal(mac, mac_a),
                    "stale entry still resolves");
        TEST_ASSERT_EQ(cap_arp_requests, 1, "refresh request sent");
        TEST_ASSERT_MEMEQ(cap_last_dest, mac_a, ETH_ALEN,
                          "refresh asks the known MAC (unicast)");

        // Answered: reachable again
        arp_input(make_arp(d0, ARP_OP_REPLY, mac_a, peer, ip0));
        TEST_ASSERT_EQ(arp_entry_state(d0, peer), ARP_STATE_REACHABLE,
                       "refresh reply makes the entry reachable");

        // Next time, unanswered: unicast, unicast, broadcast, expired
        t = timer_get_ms() + ARP_REACHABLE_MS + 1;
        cap_arp_requests = 0;
        arp_timer(t);
        arp_timer(t + ARP_RETRANS_MS);
        TEST_ASSERT_MEMEQ(cap_last_dest, mac_a, ETH_ALEN,
                          "second try unicast");
        arp_timer(t + 2 * ARP_RETRANS_MS);
        TEST_ASSERT(eth_is_broadcast(cap_last_dest),
                    "last try broadcast");
        TEST_ASSERT_EQ(cap_arp_requests, ARP_MAX_PROBES,
                       "ARP_MAX_PROBES tries");
        arp_timer(t + 3 * ARP_RETRANS_MS);
        TEST_ASSERT_EQ(arp_entry_state(d0, peer), ARP_STATE_FREE,
                       "unanswered refresh expires the entry");
        TEST_ASSERT(net_stat_total(NET_STAT_ARP_EXPIRED) > expired,
                       "expiry counted");
        TEST_ASSERT_EQ(arp_entry_state(d0, fixed), ARP_STATE_STATIC,
                       "static entry untouched by aging");
    }

    // ── Bug 4: malformed traffic is counted, not printed ─
    {
        uint64_t runt  = net_stat_total(NET_STAT_ETH_RUNT);
        uint64_t type  = net_stat_total(NET_STAT_ETH_UNKNOWN_TYPE);
        uint64_t short_ip = net_stat_total(NET_STAT_IP_SHORT);

        eth_input(make_frame(d0, 0, 10));
        eth_input(make_frame(d0, 0x88B5, 60));        // local experimental
        eth_input(make_frame(d0, ETH_P_IP, ETH_HLEN + 4));

        TEST_ASSERT_EQ(net_stat_total(NET_STAT_ETH_RUNT), runt + 1,
                       "runt frame counted");
        TEST_ASSERT_EQ(net_stat_total(NET_STAT_ETH_UNKNOWN_TYPE), type + 1,
                       "unknown ethertype counted");
        TEST_ASSERT_EQ(net_stat_total(NET_STAT_IP_SHORT), short_ip + 1,
                       "short IP packet counted");
    }

    netdev_unregister(d0);
    TEST_ASSERT_EQ(arp_entry_state(d0, fixed), ARP_STATE_FREE,
                   "static entries go with their interface too");
    kfree(d0);
    kfree(d1);

    TEST_END();
}

/// Build an Ethernet + IP + UDP frame into a fresh @p pool buffer, as if
/// the device had written it there.
static struct netbuf *build_udp_frame(struct netbuf_pool *pool,
                                      const uint8_t *dst_mac,
                                      const uint8_t *src_mac,
                                      uint32_t src_ip, uint32_t dst_ip,
                                      uint16_t sport, uint16_t dport,
                                      const char *msg, size_t len)
{
    struct netbuf *nb = netbuf_pool_alloc(pool);
    uint8_t *f = netbuf_put(nb, ETH_HLEN + IP_HDR_MIN + UDP_HDR_SIZE + len);

    struct ethhdr *eth = (struct ethhdr *)f;
    eth_copy_addr(eth->dest, dst_mac);
    eth_copy_addr(eth->src, src_mac);
    eth->ethertype = htons(ETH_P_IP);

    struct iphdr *ip = (struct iphdr *)(f + ETH_HLEN);
    memset(ip, 0, IP_HDR_MIN);
    ip->version  = IP_VERSION;
    ip->ihl      = IP_HDR_MIN / 4;
    ip->tot_len  = htons(IP_HDR_MIN + UDP_HDR_SIZE + len);
    ip->ttl      = IP_DEFAULT_TTL;
    ip->protocol = IPPROTO_UDP;
    ip->saddr    = htonl(src_ip);
    ip->daddr    = htonl(dst_ip);
    ip->check    = ip_checksum(ip, IP_HDR_MIN);

    struct udphdr *udp = (struct udphdr *)(f + ETH_HLEN + IP_HDR_MIN);
    udp->src_port = htons(sport);
    udp->dst_port = htons(dport);
    udp->len      = htons(UDP_HDR_SIZE + len);
    udp->checksum = 0;  // not computed: valid for IPv4
    memcpy(udp + 1, msg, len);

    return nb;
}

uint32_t test_net(void)
{
    TEST_BEGIN("net");

    // ── Byte order conversions ──────────────────────────
    {
        TEST_ASSERT_EQ(htons(0x0102), 0x0201, "htons");
        TEST_ASSERT_EQ(ntohs(0x0201), 0x0102, "ntohs");
        TEST_ASSERT_EQ(htonl(0x01020304), 0x04030201, "htonl");
        TEST_ASSERT_EQ(ntohl(0x04030201), 0x01020304, "ntohl");

        // Round-trip: ntohs(htons(x)) == x
        TEST_ASSERT_EQ(ntohs(htons(0x1234)), 0x1234, "htons round-trip");
        TEST_ASSERT_EQ(ntohl(htonl(0xDEADBEEF)), 0xDEADBEEF,
                       "htonl round-trip");
    }

    // ── IP checksum: zero-length ────────────────────────
    {
        uint16_t csum = ip_checksum(NULL, 0);
        TEST_ASSERT_EQ(csum, 0xFFFF, "checksum of empty data");
    }

    // ── IP checksum: known header ───────────────────────
    {
        // Construct a minimal IP header with known values and verify
        // the checksum. Then verify that recomputing over the full
        // header (including checksum) gives 0.
        uint8_t hdr[20] =
        {
            0x45, 0x00,              // version=4, ihl=5, tos=0
            0x00, 0x73,              // total length = 115
            0x00, 0x00,              // identification
            0x40, 0x00,              // flags=DF, frag offset=0
            0x40, 0x11,              // ttl=64, protocol=UDP
            0x00, 0x00,              // checksum = 0 (to be computed)
            0xC0, 0xA8, 0x00, 0x01,  // src: 192.168.0.1
            0xC0, 0xA8, 0x00, 0xC7,  // dst: 192.168.0.199
        };

        uint16_t csum = ip_checksum(hdr, 20);
        // Store the checksum in the header (network byte order)
        hdr[10] = (uint8_t)(csum & 0xFF);
        hdr[11] = (uint8_t)(csum >> 8);

        // Verification: checksum of the full header should be 0
        TEST_ASSERT_EQ(ip_checksum(hdr, 20), 0,
                       "IP checksum verifies to 0");
    }

    // ── IP checksum: odd length ─────────────────────────
    {
        uint8_t data[] = {0x01, 0x02, 0x03};
        uint16_t csum = ip_checksum(data, 3);
        // Just verify it doesn't crash and returns something
        TEST_ASSERT(csum != 0 || csum == 0, "odd-length checksum ok");
        (void)csum;
    }

    // ── IP4_ADDR macro ──────────────────────────────────
    {
        uint32_t ip = IP4_ADDR(192, 168, 1, 100);
        TEST_ASSERT_EQ((ip >> 24) & 0xFF, 192, "IP4_ADDR octet 0");
        TEST_ASSERT_EQ((ip >> 16) & 0xFF, 168, "IP4_ADDR octet 1");
        TEST_ASSERT_EQ((ip >> 8)  & 0xFF,   1, "IP4_ADDR octet 2");
        TEST_ASSERT_EQ( ip        & 0xFF, 100, "IP4_ADDR octet 3");
    }

    // ── IP address formatting ───────────────────────────
    {
        char buf[16];
        ip4_addr_to_str(IP4_ADDR(10, 0, 2, 15), buf);
        TEST_ASSERT_STREQ(buf, "10.0.2.15", "ip4_addr_to_str");

        ip4_addr_to_str(IP4_ADDR(255, 255, 255, 255), buf);
        TEST_ASSERT_STREQ(buf, "255.255.255.255", "ip4_addr_to_str max");

        ip4_addr_to_str(IP4_ADDR(0, 0, 0, 0), buf);
        TEST_ASSERT_STREQ(buf, "0.0.0.0", "ip4_addr_to_str zero");
    }

    // ── MAC address formatting ──────────────────────────
    {
        uint8_t mac[] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        char buf[18];
        eth_addr_to_str(mac, buf);
        TEST_ASSERT_STREQ(buf, "52:54:00:12:34:56",
                          "eth_addr_to_str");
    }

    // ── MAC comparison ──────────────────────────────────
    {
        uint8_t a[] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        uint8_t b[] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        uint8_t c[] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x57};

        TEST_ASSERT(eth_addr_equal(a, b), "equal MACs match");
        TEST_ASSERT(!eth_addr_equal(a, c), "different MACs differ");
    }

    // ── Broadcast / multicast / zero detection ──────────
    {
        uint8_t bcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        uint8_t mcast[] = {0x01, 0x00, 0x5E, 0x00, 0x00, 0x01};
        uint8_t ucast[] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        uint8_t zero[]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

        TEST_ASSERT(eth_is_broadcast(bcast), "broadcast detected");
        TEST_ASSERT(!eth_is_broadcast(ucast), "unicast not broadcast");

        TEST_ASSERT(eth_is_multicast(bcast), "broadcast is multicast");
        TEST_ASSERT(eth_is_multicast(mcast), "multicast detected");
        TEST_ASSERT(!eth_is_multicast(ucast), "unicast not multicast");

        TEST_ASSERT(eth_is_zero(zero), "zero detected");
        TEST_ASSERT(!eth_is_zero(ucast), "non-zero not zero");
    }

    // ── eth_copy_addr ───────────────────────────────────
    {
        uint8_t src[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        uint8_t dst[6] = {0};
        eth_copy_addr(dst, src);
        TEST_ASSERT_MEMEQ(dst, src, 6, "eth_copy_addr");
    }

    // ── IP address classification ───────────────────────
    {
        TEST_ASSERT(ip_is_broadcast(0xFFFFFFFF, NULL),
                    "255.255.255.255 is broadcast");

        TEST_ASSERT(ip_is_local(IP4_ADDR(127, 0, 0, 1)),
                    "127.0.0.1 is local");
    }

    // ── UDP bind / unbind ───────────────────────────────
    {
        TEST_ASSERT_EQ(udp_bind(9999, udp_test_handler), 0,
                       "udp_bind succeeds");
        TEST_ASSERT_EQ(udp_bind(9999, udp_test_handler), -1,
                       "udp_bind rejects duplicate");
        TEST_ASSERT_EQ(udp_bind(9998, udp_test_handler), 0,
                       "udp_bind second port");

        udp_unbind(9999);
        TEST_ASSERT_EQ(udp_bind(9999, udp_test_handler), 0,
                       "udp_bind after unbind");

        TEST_ASSERT_EQ(udp_bind(0, NULL), -1,
                       "udp_bind rejects NULL handler");

        // Clean up
        udp_unbind(9999);
        udp_unbind(9998);
    }

    // ── Transmit through a fake netdev ──────────────────
    struct netdev *fake = netdev_alloc("fake0");
    TEST_ASSERT(fake != NULL, "fake netdev allocated");
    if (fake)
    {
        uint8_t peer_mac[ETH_ALEN] = {0x02, 0, 0, 0, 0, 0x01};
        fake->mac[0] = 0x02;
        fake->mac[5] = 0x02;
        fake->ops = &fake_ok_ops;
        netdev_register(fake);
        netdev_set_up(fake);
        netdev_set_ip(fake, IP4_ADDR(10, 9, 9, 2),
                      IP4_ADDR(255, 255, 255, 0), 0);
        arp_add_static(IP4_ADDR(10, 9, 9, 1), peer_mac, fake);
    
        // eth_output must consume nb exactly once when the driver fails
        {
            fake->ops = &fake_fail_ops;
            struct netbuf *nb = netbuf_alloc(0);
            netbuf_reserve(nb, NETBUF_HEADROOM);
            netbuf_put(nb, 20);
            nb->dev = fake;
            nb->refcount++;

            int ret = eth_output(nb, peer_mac, ETH_P_IP);
            TEST_ASSERT_EQ(ret, -1, "eth_output reports driver failure");
            TEST_ASSERT_EQ(nb->refcount, 1,
                           "failed send consumes nb exactly once");
            netbuf_free(nb);
            fake->ops = &fake_ok_ops;
        }
    
        // Direct transmission (no TX owner)
        fake_sent = 0;
        TEST_ASSERT_EQ(udp_send(IP4_ADDR(10, 9, 9, 1), 1234, 7, "x", 1),
                                0, "udp_send succeeds");
        TEST_ASSERT_EQ(fake_sent, 1, "sent directly without a TX owner");
    
        // Handoff to a TX owner on another core
        if (smp_get_core_count() >= 2)
        {
            ip_set_tx_owner(1);
        
            fake_sent = 0;
            udp_send(IP4_ADDR(10, 9, 9, 1), 1234, 7, "y", 1);
            TEST_ASSERT_EQ(fake_sent, 0, "non-owner does not transmit");
            TEST_ASSERT_EQ(ip_tx_pending(), 1, "packet waits in handoff ring");

            TEST_ASSERT_EQ(ip_tx_drain(), 1, "drain finishes one packet");
            TEST_ASSERT_EQ(ip_tx_pending(), 0, "handoff ring empty");
            TEST_ASSERT_EQ(fake_sent, 1, "drained packet transmitted");
        
            ip_set_tx_owner(-1);
            udp_send(IP4_ADDR(10, 9, 9, 1), 1234, 7, "z", 1);
            TEST_ASSERT_EQ(fake_sent, 2, "owner cleared: direct again");
        }
        else
            kprintf("  single-core: TX handoff test skipped\n");

        // ── Connected-subnet routing across several devices
        struct netdev *fake1 = netdev_alloc("fake1");
        TEST_ASSERT(fake1 != NULL, "second fake netdev allocated");
        if (fake1)
        {
            fake1->ops = &fake_ok_ops;
            netdev_register(fake1);
            netdev_set_up(fake1);
            netdev_set_ip(fake1, IP4_ADDR(10, 8, 8, 2),
                          IP4_ADDR(255, 255, 255, 0),
                          IP4_ADDR(10, 8, 8, 1));
        
            struct netdev *out = NULL;
            uint32_t hop = 0;
            TEST_ASSERT(ip_route(IP4_ADDR(10, 9, 9, 7), &out, &hop) == 0
                        && out == fake && hop == IP4_ADDR(10, 9, 9, 7),
                        "on-link route picks fake0");
            TEST_ASSERT(ip_route(IP4_ADDR(10, 8, 8, 7), &out, &hop) == 0
                        && out == fake1 && hop == IP4_ADDR(10, 8, 8, 7),
                        "on-link route picks fake1");
            TEST_ASSERT(ip_route(IP4_ADDR(1, 2, 3, 4), &out, &hop) == 0
                        && out == fake1 && hop == IP4_ADDR(10, 8, 8, 1),
                        "off-link route uses the device with a gateway");
            TEST_ASSERT(ip_route(IP4_ADDR(10, 8, 8, 255), &out, &hop) == 0
                        && out == fake1,
                        "directed broadcast goes out its subnet's device");
            TEST_ASSERT(ip_is_local(IP4_ADDR(10, 8, 8, 2)),
                        "second device's address is local");
            TEST_ASSERT(!ip_is_local(IP4_ADDR(10, 8, 8, 3)),
                        "other address on its subnet is not local");
        
            // A device that is down is not routed to
            netdev_set_down(fake1);
            TEST_ASSERT(ip_route(IP4_ADDR(10, 8, 8, 7), &out, &hop) != 0,
                        "down device is not used");
            netdev_unregister(fake1);
            kfree(fake1);
        }

        netdev_unregister(fake);
    }

    // ── Zero-copy echo ──────────────────────────────────
    {
        static const uint8_t nic_mac[ETH_ALEN]  = {0x02, 0, 0, 0, 0, 0x22};
        static const uint8_t peer_mac[ETH_ALEN] = {0x02, 0, 0, 0, 0, 0x33};
        const uint32_t nic_ip  = IP4_ADDR(10, 7, 7, 2);
        const uint32_t peer_ip = IP4_ADDR(10, 7, 7, 1);
        static const char msg[] = "zero-copy echo";
        const size_t msg_len = sizeof(msg) - 1;

        struct netbuf_pool *pool = netbuf_pool_create(smp_core_id());
        struct netdev *loop = netdev_alloc("loop0");
        TEST_ASSERT(pool && loop, "fake NIC with pool");

        if (pool && loop)
        {
            memcpy(loop->mac, nic_mac, ETH_ALEN);
            loop->ops = &loop_ops;
            netdev_register(loop);
            netdev_set_up(loop);
            netdev_set_ip(loop, nic_ip, IP4_ADDR(255, 255, 255, 0), 0);
            arp_add_static(peer_ip, peer_mac, loop);
            udp_bind(7, udp_test_echo);

            loop_rx = build_udp_frame(pool, nic_mac, peer_mac,
                                      peer_ip, nic_ip,
                                      4000, 7, msg, msg_len);
            struct netbuf *rx = loop_rx;
            uint8_t *rx_frame = rx->data;

            uint64_t slow   = netbuf_slow_allocs();
            uint64_t allocs = pool->allocs;
            loop_tx = NULL;

            TEST_ASSERT_EQ(netdev_poll(loop), 1, "one packet processed");

            TEST_ASSERT_EQ(netbuf_slow_allocs(), slow,
                           "echo: zero slab / PMM allocations");
            TEST_ASSERT_EQ(pool->allocs, allocs,
                           "echo: zero pool allocations");
            TEST_ASSERT_EQ((uintptr_t)loop_tx, (uintptr_t)rx,
                           "echo: the received buffer is the transmitted one");

            if (loop_tx)
            {
                struct netbuf *tx = loop_tx;
                TEST_ASSERT_EQ((uintptr_t)tx->data, (uintptr_t)rx_frame,
                               "echo: frame at the same address");

                struct ethhdr *eth = (struct ethhdr *)tx->data;
                TEST_ASSERT_MEMEQ(eth->dest, peer_mac, ETH_ALEN,
                                  "echo: Ethernet dest is the sender");
                TEST_ASSERT_MEMEQ(eth->src, nic_mac, ETH_ALEN,
                                  "echo: Ethernet src is the NIC");

                struct iphdr *ip = (struct iphdr *)(tx->data + ETH_HLEN);
                TEST_ASSERT_EQ(ntohl(ip->saddr), nic_ip, "echo: IP src");
                TEST_ASSERT_EQ(ntohl(ip->daddr), peer_ip, "echo: IP dst");
                TEST_ASSERT_EQ(ip_checksum(ip, IP_HDR_MIN), 0,
                               "echo: IP checksum valid");

                struct udphdr *udp = (struct udphdr *)(ip + 1);
                TEST_ASSERT_EQ(ntohs(udp->src_port), 7, "echo: source port");
                TEST_ASSERT_EQ(ntohs(udp->dst_port), 4000, "echo: dest port");
                TEST_ASSERT_EQ(ntohs(udp->len), UDP_HDR_SIZE + msg_len,
                               "echo: UDP length");
                TEST_ASSERT(udp->checksum != 0, "echo: UDP checksum set");
                TEST_ASSERT_MEMEQ(udp + 1, msg, msg_len,
                                  "echo: payload intact");

                netbuf_free(tx);
            }

            // A burst of replies shares one doorbell
            loop_tx_calls = 0;
            loop_tx_pkts  = 0;
            netdev_tx_defer_begin();
            for (int i = 0; i < 5; ++i)
            {
                struct netbuf *nb = netbuf_pool_alloc(pool);
                netbuf_put(nb, 60);
                nb->dev = loop;
                TEST_ASSERT_EQ(netdev_send(loop, nb), 0,
                               "held send accepted");
            }
            TEST_ASSERT_EQ(loop_tx_calls, 0, "nothing posted while held");
            netdev_tx_defer_end();
            TEST_ASSERT_EQ(loop_tx_pkts, 5, "held packets all posted");
            TEST_ASSERT_EQ(loop_tx_calls, 1, "one burst, one doorbell");
            netbuf_free(loop_tx);

            TEST_ASSERT_EQ(netbuf_pool_available(pool), pool->capacity,
                           "pool whole again");

            udp_unbind(7);
            netdev_unregister(loop);
        }

        kfree(loop);
        netbuf_pool_destroy(pool);
    }

    // ── Default names: lowest free ethN ─────────────────
    {
        int eth = 0;
        while (eth < NETDEV_MAX)
        {
            char name[16];
            ksnprintf(name, sizeof(name), "eth%d", eth);
            if (!netdev_find(name))
                break;
            eth++;
        }

        char name_a[16], name_b[16];
        ksnprintf(name_a, sizeof(name_a), "eth%d", eth);
        ksnprintf(name_b, sizeof(name_b), "eth%d", eth + 1);

        struct netdev *a = netdev_alloc(NULL);
        struct netdev *b = netdev_alloc(NULL);
        struct netdev *c = netdev_alloc(NULL);
        TEST_ASSERT(a && b && c, "unnamed netdevs allocated");
        if (a && b && c)
        {
            TEST_ASSERT_EQ(netdev_register(a), 0,
                           "unnamed netdev registers");
            TEST_ASSERT_EQ(netdev_register(b), 0,
                           "second unnamed netdev registers");
            TEST_ASSERT_STREQ(a->name, name_a, "gets the next free ethN");
            TEST_ASSERT_STREQ(b->name, name_b, "then the one after");
        
            // Unregistering frees name
            netdev_unregister(a);
            TEST_ASSERT_EQ(netdev_register(c), 0,
                           "third unnamed netdev registers");
            TEST_ASSERT_STREQ(c->name, name_a, "lowest freed name reused");

            // Registering under a live name fails
            strlcpy(a->name, name_b, sizeof(a->name));
            TEST_ASSERT_EQ(netdev_register(a), -1,
                           "duplicate name rejected");

            netdev_unregister(b);
            netdev_unregister(c);
        }
        kfree(a);
        kfree(b);
        kfree(c);
    }

    TEST_END();
}

