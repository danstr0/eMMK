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
#include "pmm.h"
#include "smp.h"

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
        TEST_ASSERT(frame != NULL, "put for pull test");
        if (frame)
        {
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
        }

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

    // ── Layout: one allocation, device headroom below head
    {
        struct netbuf *nb = netbuf_alloc(0);
        TEST_ASSERT_EQ((uintptr_t)nb->head,
                       (uintptr_t)nb + NETBUF_DATA_OFFSET,
                       "metadata embedded in front of the data");
        TEST_ASSERT_EQ((size_t)(nb->end - nb->head), NETBUF_DATA_SIZE,
                       "default data size");
        TEST_ASSERT_EQ(nb->phys_addr,
                       VIRT_TO_PHYS_ADDR((uintptr_t)nb) + NETBUF_DATA_OFFSET,
                       "phys_addr is that of head");
    
        netbuf_reserve(nb, 34);
        TEST_ASSERT_EQ(netbuf_data_phys(nb), nb->phys_addr + 34,
                       "netbuf_data_phys follows data");
        netbuf_free(nb);
    }

    // ── Oversized buffers come from the PMM ─────────────
    {
        struct pmm_stats before, after;
        pmm_get_stats(&before);

        struct netbuf *nb = netbuf_alloc(8192);
        TEST_ASSERT(nb != NULL, "oversized alloc");
        if (nb)
        {
            TEST_ASSERT_EQ((size_t)(nb->end - nb->head), 8192,
                           "oversized size");
            memset(netbuf_put(nb, 8192), 0x5A, 8192);
            netbuf_free(nb);
        }

        pmm_get_stats(&after);
        TEST_ASSERT_EQ(after.free_pages, before.free_pages,
                       "oversized free returns its pages");
    }

    // ── Pools ───────────────────────────────────────────
    {
        struct pmm_stats before, after;
        pmm_get_stats(&before);

        int me = smp_core_id();
        struct netbuf_pool *pool = netbuf_pool_create(me);
        TEST_ASSERT(pool != NULL, "pool created");
    
        if (pool)
        {
            TEST_ASSERT_EQ(pool->phys & (NETBUF_POOL_BLOCK_SIZE - 1), 0,
                           "pool block is 2 MiB aligned");
            TEST_ASSERT(pool->capacity >= 1000, "pool holds ~1000 buffers");
            TEST_ASSERT_EQ(netbuf_pool_available(pool), pool->capacity,
                           "all buffers free at first");
        
            // Allocation does not touch the allocators
            uint64_t slow = netbuf_slow_allocs();
            struct netbuf *a = netbuf_pool_alloc(pool);
            TEST_ASSERT(a != NULL, "pool alloc");
            TEST_ASSERT_EQ(netbuf_slow_allocs(), slow, "pool alloc is not slow");
            TEST_ASSERT_EQ(a->refcount, 1, "pool buffer: refcount 1");
            TEST_ASSERT(a->pool == pool, "pool buffer knows its pool");
            TEST_ASSERT_EQ(a->phys_addr,
                           VIRT_TO_PHYS_ADDR((uintptr_t)a) + NETBUF_DATA_OFFSET,
                           "pool buffer phys_addr");
            TEST_ASSERT_EQ((uintptr_t)a->data, (uintptr_t)a->head,
                           "pool buffer starts empty");
        
            // Metadata is reset on reuse; LIFO hands back the same buffer
            netbuf_reserve(a, 20);
            netbuf_put(a, 100);
            a->src_ip = 0x01020304;
            netbuf_free(a);
            struct netbuf *b = netbuf_pool_alloc(pool);
            TEST_ASSERT_EQ((uintptr_t)b, (uintptr_t)a, "LIFO: hot buffer reused");
            TEST_ASSERT(b->len == 0 && b->src_ip == 0 && b->data == b->head,
                        "metadata reset on reuse");

            // Double free is caught, not stacked twice
            netbuf_free(b);
            uint32_t avail = netbuf_pool_available(pool);
            netbuf_free(b);
            TEST_ASSERT_EQ(netbuf_pool_available(pool), avail,
                           "double free ignored");
        
            // Exhaustion fails cleanly and recovers
            static struct netbuf *all[NETBUF_POOL_MAX];
            uint32_t n = 0;
            while ((all[n] = netbuf_pool_alloc(pool)) != NULL)
                n++;
            TEST_ASSERT_EQ(n, pool->capacity, "every buffer allocatable");
            TEST_ASSERT(pool->alloc_fails == 1, "empty pool reports failure");

            bool unique = true;
            for (uint32_t i = 1; i < n; ++i)
                if ((uintptr_t)all[i] - (uintptr_t)all[i - 1] != NETBUF_SIZE)
                    unique = false;
            TEST_ASSERT(unique, "buffers are distinct and contiguous");

            // A free on another core goes through the remote list
            pool->owner_core = me + 1;
            netbuf_free(all[0]);
            netbuf_free(all[1]);
            pool->owner_core = me;
            TEST_ASSERT_EQ(pool->remote_frees, 2, "remote frees counted");
            TEST_ASSERT_EQ(pool->free_count, 0, "not on the owner's stack yet");

            struct netbuf *c = netbuf_pool_alloc(pool);
            TEST_ASSERT(c == all[0] || c == all[1],
                        "owner reclaims remote frees when empty");
            netbuf_free(c);
            for (uint32_t i = 2; i < n; ++i)
                netbuf_free(all[i]);
            TEST_ASSERT_EQ(netbuf_pool_available(pool), pool->capacity,
                           "all buffers back");

            netbuf_pool_destroy(pool);
        }

        pmm_get_stats(&after);
        TEST_ASSERT_EQ(after.free_pages, before.free_pages,
                       "pool destroy returns the block");
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

