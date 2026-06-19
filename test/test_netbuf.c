/**
 * @file test_netbuf.c
 * @brief Tests for the network buffer (netbuf) subsystem.
 *
 * Tests buffer allocation, pointer manipulation (push/pull/put/trim),
 * copy with header remapping, and queue operations. These are the most
 * important networking tests because silent buffer corruption produces
 * packets that look valid but carry wrong data.
 */

#include "test.h"
#include "netbuf.h"

uint32_t test_netbuf(void)
{
    TEST_BEGIN("netbuf");

    // ── Alloc / free ────────────────────────────────────
    {
        struct netbuf *nb = netbuf_alloc(0);
        TEST_ASSERT(nb != NULL, "alloc default size");
        TEST_ASSERT(nb->head != NULL, "head non-NULL");
        TEST_ASSERT_EQ((uintptr_t)nb->data, (uintptr_t)nb->head,
                       "data == head initially");
        TEST_ASSERT_EQ((uintptr_t)nb->tail, (uintptr_t)nb->head,
                       "tail == head initially");
        TEST_ASSERT_EQ(nb->len, 0, "len starts at 0");
        TEST_ASSERT_EQ(nb->refcount, 1, "refcount starts at 1");
        netbuf_free(nb);
    }

    // ── Reserve headroom ────────────────────────────────
    {
        struct netbuf *nb = netbuf_alloc(0);
        netbuf_reserve(nb, 64);

        TEST_ASSERT_EQ(netbuf_headroom(nb), 64, "headroom after reserve");
        TEST_ASSERT_EQ(nb->len, 0, "len unchanged by reserve");
        TEST_ASSERT_EQ((uintptr_t)nb->data, (uintptr_t)nb->tail,
                       "data == tail after reserve (empty)");
        netbuf_free(nb);
    }

    // ── Put (append data) ───────────────────────────────
    {
        struct netbuf *nb = netbuf_alloc(0);
        netbuf_reserve(nb, 64);

        void *p = netbuf_put(nb, 100);
        TEST_ASSERT(p != NULL, "put returns non-NULL");
        TEST_ASSERT_EQ(nb->len, 100, "len after put");
        TEST_ASSERT_EQ(netbuf_tailroom(nb),
                       (size_t)(nb->end - nb->tail),
                       "tailroom consistent");

        // Write a pattern and verify
        memset(p, 0xAA, 100);
        TEST_ASSERT_EQ(nb->data[0], 0xAA, "put region writable");
        TEST_ASSERT_EQ(nb->data[99], 0xAA, "put region end writable");

        netbuf_free(nb);
    }

    // ── Push (prepend header) ───────────────────────────
    {
        struct netbuf *nb = netbuf_alloc(0);
        netbuf_reserve(nb, 64);
        netbuf_put(nb, 100);

        void *hdr = netbuf_push(nb, 14);
        TEST_ASSERT(hdr != NULL, "push returns non-NULL");
        TEST_ASSERT_EQ(nb->len, 114, "len after push");
        TEST_ASSERT_EQ(netbuf_headroom(nb), 50,
                       "headroom reduced by push");
        TEST_ASSERT_EQ((uintptr_t)hdr, (uintptr_t)nb->data,
                       "push returns new data start");

        netbuf_free(nb);
    }

    // ── Pull (strip header) ─────────────────────────────
    {
        struct netbuf *nb = netbuf_alloc(0);
        netbuf_reserve(nb, 14);

        // Simulate a received Ethernet frame
        uint8_t *frame = netbuf_put(nb, 64);
        memset(frame, 0xBB, 64);
        frame[0] = 0xDE;  // marker in the "header"
        frame[14] = 0xAD; // marker in the "payload"

        void *old = netbuf_pull(nb, 14);
        TEST_ASSERT(old != NULL, "pull returns non-NULL");
        TEST_ASSERT_EQ(((uint8_t *)old)[0], 0xDE,
                       "pull returns old data start");
        TEST_ASSERT_EQ(nb->data[0], 0xAD,
                       "data now points to payload");
        TEST_ASSERT_EQ(nb->len, 50, "len after pull");

        netbuf_free(nb);
    }

    // ── Trim (remove from tail) ─────────────────────────
    {
        struct netbuf *nb = netbuf_alloc(0);
        netbuf_reserve(nb, 64);
        netbuf_put(nb, 100);

        netbuf_trim(nb, 20);
        TEST_ASSERT_EQ(nb->len, 80, "len after trim");

        netbuf_free(nb);
    }

    // ── set_len ─────────────────────────────────────────
    {
        struct netbuf *nb = netbuf_alloc(0);
        netbuf_reserve(nb, 64);
        netbuf_put(nb, 200);

        netbuf_set_len(nb, 100);
        TEST_ASSERT_EQ(nb->len, 100, "len after set_len shrink");
        TEST_ASSERT_EQ((uintptr_t)nb->tail,
                       (uintptr_t)(nb->data + 100),
                       "tail consistent after set_len");

        netbuf_free(nb);
    }

    // ── TX header construction sequence ─────────────────
    {
        struct netbuf *nb = netbuf_alloc(0);

        // Reserve headroom for ETH(14) + IP(20)
        netbuf_reserve(nb, 34);

        // Put payload (50 bytes of application data)
        uint8_t *payload = netbuf_put(nb, 50);
        TEST_ASSERT(payload != NULL, "tx: put payload");
        memset(payload, 'X', 50);

        // Push IP header
        uint8_t *ip_hdr = netbuf_push(nb, 20);
        TEST_ASSERT(ip_hdr != NULL, "tx: push IP");
        TEST_ASSERT_EQ(nb->len, 70, "tx: len after IP push");

        // Push Ethernet header
        uint8_t *eth_hdr = netbuf_push(nb, 14);
        TEST_ASSERT(eth_hdr != NULL, "tx: push ETH");
        TEST_ASSERT_EQ(nb->len, 84, "tx: len after ETH push");
        TEST_ASSERT_EQ(netbuf_headroom(nb), 0,
                       "tx: headroom fully consumed");

        // Verify the data region is contiguous: ETH | IP | payload
        TEST_ASSERT_EQ((uintptr_t)nb->data, (uintptr_t)eth_hdr,
                       "tx: data == eth_hdr");
        TEST_ASSERT_EQ((uintptr_t)(eth_hdr + 14), (uintptr_t)ip_hdr,
                       "tx: ip follows eth");
        TEST_ASSERT_EQ((uintptr_t)(ip_hdr + 20), (uintptr_t)payload,
                       "tx: payload follows ip");

        netbuf_free(nb);
    }

    // ── Copy with header pointer remapping ──────────────
    {
        struct netbuf *nb = netbuf_alloc(0);
        netbuf_reserve(nb, 34);
        uint8_t *payload = netbuf_put(nb, 50);
        memset(payload, 'Z', 50);
        netbuf_push(nb, 20);
        nb->network_header = nb->data;
        netbuf_push(nb, 14);
        nb->mac_header = nb->data;
        nb->protocol = ETH_P_IP;
        nb->src_ip = 0x0A000201;

        struct netbuf *copy = netbuf_copy(nb);
        TEST_ASSERT(copy != NULL, "copy non-NULL");
        TEST_ASSERT_EQ(copy->len, nb->len, "copy: same len");
        TEST_ASSERT_EQ(copy->protocol, ETH_P_IP, "copy: protocol");
        TEST_ASSERT_EQ(copy->src_ip, 0x0A000201, "copy: src_ip");

        // Header pointers should be remapped into the copy's buffer
        TEST_ASSERT(copy->mac_header != nb->mac_header,
                    "copy: mac_header remapped");
        TEST_ASSERT(copy->network_header != nb->network_header,
                    "copy: network_header remapped");

        // ...but the offsets from head should be the same
        size_t mac_off_orig = (uint8_t *)nb->mac_header - nb->head;
        size_t mac_off_copy = (uint8_t *)copy->mac_header - copy->head;
        TEST_ASSERT_EQ(mac_off_orig, mac_off_copy,
                       "copy: mac_header offset preserved");

        // Data content should match
        TEST_ASSERT_MEMEQ(nb->data, copy->data, nb->len,
                          "copy: data matches");

        netbuf_free(copy);
        netbuf_free(nb);
    }

    // ── Queue operations ────────────────────────────────
    {
        struct netbuf_queue q;
        netbuf_queue_init(&q);

        TEST_ASSERT(netbuf_queue_empty(&q), "queue starts empty");
        TEST_ASSERT_EQ(netbuf_queue_len(&q), 0, "queue len 0");

        struct netbuf *a = netbuf_alloc(0);
        struct netbuf *b = netbuf_alloc(0);
        struct netbuf *c = netbuf_alloc(0);

        netbuf_queue_push(&q, a);
        netbuf_queue_push(&q, b);
        netbuf_queue_push(&q, c);
        TEST_ASSERT_EQ(netbuf_queue_len(&q), 3, "queue len 3");

        struct netbuf *out = netbuf_queue_pop(&q);
        TEST_ASSERT_EQ((uintptr_t)out, (uintptr_t)a, "FIFO: first out");
        TEST_ASSERT_EQ(netbuf_queue_len(&q), 2, "queue len 2");

        out = netbuf_queue_pop(&q);
        TEST_ASSERT_EQ((uintptr_t)out, (uintptr_t)b, "FIFO: second out");

        // Purge remainder
        netbuf_queue_purge(&q);
        TEST_ASSERT(netbuf_queue_empty(&q), "queue empty after purge");

        netbuf_free(a);
        netbuf_free(b);
    }

    // ── Overflow / underflow guards ─────────────────────
    {
        struct netbuf *nb = netbuf_alloc(0);
        netbuf_reserve(nb, 10);
        netbuf_put(nb, 20);

        // Push more than headroom - should fail gracefully
        void *p = netbuf_push(nb, 100);
        TEST_ASSERT_EQ((uintptr_t)p, (uintptr_t)NULL,
                       "push overflow returns NULL");
        TEST_ASSERT_EQ(nb->len, 20, "len unchanged after failed push");

        // Pull more than len - should fail
        p = netbuf_pull(nb, 100);
        TEST_ASSERT_EQ((uintptr_t)p, (uintptr_t)NULL,
                       "pull underflow returns NULL");

        netbuf_free(nb);
    }

    TEST_END();
}

