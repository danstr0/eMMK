/**
 * @file test_types.c
 * @brief Tests for types.h: sizes, alignment, min/max, bit ops,
 *        @c container_of.
 */

#include "test.h"

// ── Helper struct for container_of test ────────────────────────────

struct outer
{
    uint32_t before;
    uint64_t inner;
    uint32_t after;
};

uint32_t test_types(void)
{
    TEST_BEGIN("types");

    // ── Exact-width sizes ───────────────────────────────
    TEST_ASSERT_EQ(sizeof(int8_t),    1, "int8_t is 1 byte");
    TEST_ASSERT_EQ(sizeof(uint8_t),   1, "uint8_t is 1 byte");
    TEST_ASSERT_EQ(sizeof(int16_t),   2, "int16_t is 2 bytes");
    TEST_ASSERT_EQ(sizeof(uint16_t),  2, "uint16_t is 2 bytes");
    TEST_ASSERT_EQ(sizeof(int32_t),   4, "int32_t is 4 bytes");
    TEST_ASSERT_EQ(sizeof(uint32_t),  4, "uint32_t is 4 bytes");
    TEST_ASSERT_EQ(sizeof(int64_t),   8, "int64_t is 8 bytes");
    TEST_ASSERT_EQ(sizeof(uint64_t),  8, "uint64_t is 8 bytes");
    TEST_ASSERT_EQ(sizeof(uintptr_t), 8, "uintptr_t is 8 bytes");
    TEST_ASSERT_EQ(sizeof(size_t),    8, "size_t is 8 bytes");
    TEST_ASSERT_EQ(sizeof(bool),      1, "_Bool is 1 byte");

    // ── ALIGN_UP ────────────────────────────────────────
    TEST_ASSERT_EQ(ALIGN_UP(0, 4),     0, "align_up 0 to 4");
    TEST_ASSERT_EQ(ALIGN_UP(1, 4),     4, "align_up 1 to 4");
    TEST_ASSERT_EQ(ALIGN_UP(4, 4),     4, "align_up 4 to 4 (aligned)");
    TEST_ASSERT_EQ(ALIGN_UP(5, 4),     8, "align_up 5 to 4");
    TEST_ASSERT_EQ(ALIGN_UP(0x1001, 0x1000), 0x2000,
                   "align_up page boundary");

    // ── ALIGN_DOWN ──────────────────────────────────────
    TEST_ASSERT_EQ(ALIGN_DOWN(0, 4),     0, "align_down 0 to 4");
    TEST_ASSERT_EQ(ALIGN_DOWN(3, 4),     0, "align_down 3 to 4");
    TEST_ASSERT_EQ(ALIGN_DOWN(4, 4),     4, "align_down 4 to 4 (aligned)");
    TEST_ASSERT_EQ(ALIGN_DOWN(7, 4),     4, "align_down 7 to 4");
    TEST_ASSERT_EQ(ALIGN_DOWN(0x1FFF, 0x1000), 0x1000,
                   "align_down page boundary");

    // ── MIN / MAX / CLAMP ───────────────────────────────
    TEST_ASSERT_EQ(MIN(3, 5),    3, "min(3,5)");
    TEST_ASSERT_EQ(MIN(5, 3),    3, "min(5,3)");
    TEST_ASSERT_EQ(MIN(4, 4),    4, "min equal");
    TEST_ASSERT_EQ(MAX(3, 5),    5, "max(3,5)");
    TEST_ASSERT_EQ(MAX(5, 3),    5, "max(5,3)");
    TEST_ASSERT_EQ(MAX(5, 5),    5, "max equal");
    TEST_ASSERT_EQ(CLAMP(2, 1, 10), 2,  "clamp in range");
    TEST_ASSERT_EQ(CLAMP(0, 1, 10), 1,  "clamp below");
    TEST_ASSERT_EQ(CLAMP(20, 1, 10), 10, "clamp above");

    // ── BIT macros ──────────────────────────────────────
    TEST_ASSERT_EQ(BIT(0),  1ULL,  "BIT(0)");
    TEST_ASSERT_EQ(BIT(3),  8ULL,  "BIT(3)");
    TEST_ASSERT_EQ(BIT(63), 1ULL << 63, "BIT(63)");

    {
        uint64_t x = 0;
        SET_BIT(x, 4);
        TEST_ASSERT_EQ(x, 0x10, "SET_BIT 4");
        TEST_ASSERT(TEST_BIT(x, 4), "TEST_BIT 4 set");
        TEST_ASSERT(!TEST_BIT(x, 3), "TEST_BIT 3 clear");

        TOGGLE_BIT(x, 4);
        TEST_ASSERT_EQ(x, 0, "TOGGLE_BIT clears");
        TOGGLE_BIT(x, 4);
        TEST_ASSERT_EQ(x, 0x10, "TOGGLE_BIT sets");

        CLEAR_BIT(x, 4);
        TEST_ASSERT_EQ(x, 0, "CLEAR_BIT");
    }

    // ── ARRAY_SIZE ──────────────────────────────────────
    {
        int arr[7];
        TEST_ASSERT_EQ(ARRAY_SIZE(arr), 7, "ARRAY_SIZE");
    }

    // ── container_of ────────────────────────────────────
    {
        struct outer obj;
        uint64_t *ip = &obj.inner;
        struct outer *recovered = container_of(ip, struct outer, inner);
        TEST_ASSERT_EQ((uintptr_t)recovered, (uintptr_t)&obj,
                       "container_of round-trip");
    }

    TEST_END();
}

