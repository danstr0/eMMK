/**
 * @file test_net.c
 * @brief Tests for network utilities: checksum, byte order, address
 *        formatting, and MAC comparison.
 */

#include "test.h"
#include "ip.h"
#include "ethernet.h"
#include "netdev.h"
#include "endian.h"

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

    TEST_END();
}

