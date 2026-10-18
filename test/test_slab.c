/**
 * @file test_slab.c
 * @brief Tests for the slab allocator.
 */

#include "test.h"
#include "slab.h"
#include "heap.h"

uint32_t test_slab(void)
{
    TEST_BEGIN("slab");

    // ── Create / destroy an empty cache ─────────────────
    {
        struct slab_cache *c = slab_cache_create("test-empty", 48, SLAB_NONE);
        TEST_ASSERT(c != NULL, "create cache");

        struct slab_stats st;
        slab_cache_get_stats(c, &st);
        TEST_ASSERT_EQ(st.active_count, 0, "empty cache: 0 active");
        TEST_ASSERT_EQ(st.slab_count,   0, "empty cache: 0 slabs");

        slab_cache_destroy(c);
    }

    // ── Single alloc / free ─────────────────────────────
    {
        struct slab_cache *c = slab_cache_create("test-single", 64, SLAB_NONE);
        TEST_ASSERT(c != NULL, "create single-test cache");

        void *obj = slab_alloc(c);
        TEST_ASSERT(obj != NULL, "slab_alloc non-NULL");
        TEST_ASSERT_EQ((uintptr_t)obj % CACHE_LINE_SIZE, 0,
                       "slab object aligned");

        // Verify the object is writable
        memset(obj, 0xCC, 64);
        TEST_ASSERT_EQ(((uint8_t *)obj)[0],  0xCC, "slab write byte 0");
        TEST_ASSERT_EQ(((uint8_t *)obj)[63], 0xCC, "slab write byte 63");

        struct slab_stats st;
        slab_cache_drain_magazines(c);
        slab_cache_get_stats(c, &st);
        TEST_ASSERT_EQ(st.active_count, 1, "1 active after alloc");

        slab_free(c, obj);
        slab_cache_drain_magazines(c);
        slab_cache_get_stats(c, &st);
        TEST_ASSERT_EQ(st.active_count, 0, "0 active after free");

        slab_cache_destroy(c);
    }

    // ── Multiple allocs produce distinct objects ────────
    {
        struct slab_cache *c = slab_cache_create("test-multi", 32, SLAB_NONE);
        void *a = slab_alloc(c);
        void *b = slab_alloc(c);
        void *c_obj = slab_alloc(c);

        TEST_ASSERT(a && b && c_obj, "multi alloc non-NULL");
        TEST_ASSERT(a != b && b != c_obj && a != c_obj,
                    "objects are distinct");

        slab_free(c, c_obj);
        slab_free(c, b);
        slab_free(c, a);
        slab_cache_destroy(c);
    }

    // ── SLAB_ZERO flag ──────────────────────────────────
    {
        struct slab_cache *c = slab_cache_create("test-zero", 128, SLAB_ZERO);
        void *obj = slab_alloc(c);
        TEST_ASSERT(obj != NULL, "zero-cache alloc");

        bool clean = true;
        for (size_t i = 0; i < 128; ++i)
            if (((uint8_t *)obj)[i] != 0)
            {
                clean = false;
                break;
            }
        TEST_ASSERT(clean, "SLAB_ZERO returns zeroed object");

        slab_free(c, obj);
        slab_cache_destroy(c);
    }

    // ── Reuse: freed object is returned by next alloc ───
    {
        struct slab_cache *c = slab_cache_create("test-reuse", 64, SLAB_NONE);
        void *first = slab_alloc(c);
        slab_free(c, first);
        void *second = slab_alloc(c);
        TEST_ASSERT_EQ((uintptr_t)first, (uintptr_t)second,
                       "freed object is reused");
        slab_free(c, second);
        slab_cache_destroy(c);
    }

    // ── Fill slab to trigger new slab creation ──────────
    {
        struct slab_cache *c = slab_cache_create("test-fill", 64, SLAB_NONE);

        struct slab_stats st;
        slab_cache_get_stats(c, &st);
        size_t per_slab = st.obj_count;  // 0 before first alloc

        // Force first slab creation
        void *first = slab_alloc(c);
        slab_cache_get_stats(c, &st);
        per_slab = st.obj_count;  // now reflects objs_per_slab
        TEST_ASSERT(per_slab > 0, "objs_per_slab > 0");

        // Allocate remaining objects to fill the slab
        void **objs = kmalloc(per_slab * sizeof(void *));
        TEST_ASSERT(objs != NULL, "alloc tracking array");
        objs[0] = first;
        for (size_t i = 1; i < per_slab; ++i)
            objs[i] = slab_alloc(c);

        slab_cache_drain_magazines(c);
        slab_cache_get_stats(c, &st);
        TEST_ASSERT_EQ(st.active_count, per_slab,
                       "filled first slab");

        // One more allocation should create a second slab
        void *overflow = slab_alloc(c);
        TEST_ASSERT(overflow != NULL, "overflow alloc succeeds");
        slab_cache_get_stats(c, &st);
        TEST_ASSERT(st.slab_count >= 2, "second slab created");

        // Clean up
        slab_free(c, overflow);
        for (size_t i = 0; i < per_slab; ++i)
            slab_free(c, objs[i]);
        kfree(objs);
        slab_cache_destroy(c);
    }

    // ── Shrink reclaims empty slabs ─────────────────────
    {
        struct slab_cache *c = slab_cache_create("test-shrink", 64, SLAB_NONE);
        void *obj = slab_alloc(c);
        slab_free(c, obj);

        struct slab_stats st;
        slab_cache_get_stats(c, &st);
        TEST_ASSERT(st.slab_count > 0, "has slab before shrink");

        slab_cache_drain_magazines(c);
        size_t freed = slab_cache_shrink(c);
        TEST_ASSERT(freed > 0, "shrink freed pages");

        slab_cache_get_stats(c, &st);
        TEST_ASSERT_EQ(st.slab_count, 0, "no slabs after shrink");

        slab_cache_destroy(c);
    }

    // ── Generic size-class API ──────────────────────────
    {
        void *p16  = slab_alloc_size(10);
        void *p128 = slab_alloc_size(100);
        void *p4k  = slab_alloc_size(4000);
        TEST_ASSERT(p16 && p128 && p4k, "size-class allocs non-NULL");

        slab_free_size(p4k,  4000);
        slab_free_size(p128, 100);
        slab_free_size(p16,  10);
    }

    // ── Size > 4096 falls back to kmalloc ───────────────
    {
        void *big = slab_alloc_size(8192);
        TEST_ASSERT(big != NULL, "oversized falls back to kmalloc");
        slab_free_size(big, 8192);
    }

    TEST_END();
}

