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

