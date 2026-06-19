/**
 * @file test_heap.c
 * @brief Tests for the kernel heap allocator.
 */

#include "test.h"
#include "heap.h"

uint32_t test_heap(void)
{
    TEST_BEGIN("heap");

    // ── Basic kmalloc / kfree ───────────────────────────
    {
        void *ptr = kmalloc(64);
        TEST_ASSERT(ptr != NULL, "kmalloc(64) non-NULL");

        // Verify the pointer is usable
        memset(ptr, 0xAB, 64);
        TEST_ASSERT_EQ(((uint8_t *)ptr)[0],  0xAB, "write byte 0");
        TEST_ASSERT_EQ(((uint8_t *)ptr)[63], 0xAB, "write byte 63");

        kfree(ptr);
    }

    // ── Alignment guarantee ─────────────────────────────
    {
        void *ptr = kmalloc(1);
        TEST_ASSERT(ptr != NULL, "kmalloc(1) non-NULL");
        TEST_ASSERT_EQ((uintptr_t)ptr % HEAP_ALIGNMENT, 0,
                       "aligned to HEAP_ALIGNMENT");
        kfree(ptr);
    }

    // ── kzalloc returns zeroed memory ───────────────────
    {
        void *ptr = kzalloc(128);
        TEST_ASSERT(ptr != NULL, "kzalloc(128) non-NULL");

        bool clean = true;
        for (size_t i = 0; i < 128; ++i)
            if (((uint8_t *)ptr)[i] != 0)
            {
                clean = false;
                break;
            }
        TEST_ASSERT(clean, "kzalloc memory is zeroed");
        kfree(ptr);
    }

    // ── kmalloc(0) returns NULL ─────────────────────────
    {
        TEST_ASSERT_EQ((uintptr_t)kmalloc(0), (uintptr_t)NULL,
                       "kmalloc(0) returns NULL");
    }

    // ── kfree(NULL) is safe ─────────────────────────────
    {
        kfree(NULL);  // must not crash
        TEST_ASSERT(true, "kfree(NULL) safe");
    }

    // ── Multiple allocations are distinct ───────────────
    {
        void *a = kmalloc(32);
        void *b = kmalloc(32);
        void *c = kmalloc(32);
        TEST_ASSERT(a != NULL && b != NULL && c != NULL,
                    "triple alloc non-NULL");
        TEST_ASSERT(a != b && b != c && a != c,
                    "allocations are distinct");
        kfree(c);
        kfree(b);
        kfree(a);
    }

    // ── Coalescing: free adjacent blocks, realloc larger 
    {
        void *a = kmalloc(64);
        void *b = kmalloc(64);
        void *c = kmalloc(64);
        TEST_ASSERT(a && b && c, "coalesce: alloc A/B/C");

        kfree(b);
        kfree(a);
        void *d = kmalloc(96);
        TEST_ASSERT(d != NULL, "coalesce: larger alloc after merge");

        kfree(d);
        kfree(c);
    }

    // ── krealloc: NULL ptr acts as malloc ───────────────
    {
        void *ptr = krealloc(NULL, 64);
        TEST_ASSERT(ptr != NULL, "krealloc(NULL, 64) non-NULL");
        kfree(ptr);
    }

    // ── krealloc: grow preserves content ────────────────
    {
        uint8_t *ptr = kmalloc(32);
        TEST_ASSERT(ptr != NULL, "krealloc grow: initial alloc");
        for (size_t i = 0; i < 32; ++i)
            ptr[i] = (uint8_t)i;

        uint8_t *grown = krealloc(ptr, 128);
        TEST_ASSERT(grown != NULL, "krealloc grow: success");

        bool preserved = true;
        for (size_t i = 0; i < 32; ++i)
            if (grown[i] != (uint8_t)i)
            {
                preserved = false;
                break;
            }
        TEST_ASSERT(preserved, "krealloc preserves content");
        kfree(grown);
    }

    // ── krealloc: size 0 frees ──────────────────────────
    {
        void *ptr = kmalloc(64);
        void *result = krealloc(ptr, 0);
        TEST_ASSERT_EQ((uintptr_t)result, (uintptr_t)NULL,
                       "krealloc(ptr, 0) returns NULL");
    }

    // ── kaligned_alloc ──────────────────────────────────
    {
        void *ptr = kaligned_alloc(64, 256);
        TEST_ASSERT(ptr != NULL, "kaligned_alloc non-NULL");
        TEST_ASSERT_EQ((uintptr_t)ptr % 256, 0,
                       "kaligned to 256");
        kfree(ptr);
    }

    // ── Stats consistency ───────────────────────────────
    {
        struct heap_stats s1, s2;
        heap_get_stats(&s1);

        void *ptr = kmalloc(512);
        heap_get_stats(&s2);
        TEST_ASSERT(s2.used_size > s1.used_size,
                    "alloc increases used_size");
        TEST_ASSERT_EQ(s2.num_allocs, s1.num_allocs + 1,
                       "alloc increments count");

        kfree(ptr);
        heap_get_stats(&s2);
        TEST_ASSERT_EQ(s2.num_frees, s1.num_frees + 1,
                       "free increments count");
    }

    TEST_END();
}

