/**
 * @file test_pmm.c
 * @brief Tests for the physical memory manager.
 *
 * Tests run after @c pmm_init() has been called by the test runner.
 * Each test cleans up its allocations so later tests see a consistent
 * PMM state.
 */

#include "test.h"
#include "pmm.h"

/// Maximum pages to allocate in stress tests (to avoid exhausting memory).
#define TEST_PMM_MAX_PAGES  64

uint32_t test_pmm(void)
{
    TEST_BEGIN("pmm");

    // ── Single page alloc / free ────────────────────────
    {
        uint64_t page = pmm_alloc_page();
        TEST_ASSERT_NEQ(page, PMM_ALLOC_FAILED, "alloc single page");
        TEST_ASSERT(PAGE_ALIGNED(page), "page is aligned");

        struct pmm_stats before, after;
        pmm_get_stats(&before);
        pmm_free_page(page);
        pmm_get_stats(&after);

        TEST_ASSERT_EQ(after.free_pages, before.free_pages + 1,
                       "free increments count");
    }

    // ── Contiguous allocation ───────────────────────────
    {
        uint64_t pages = pmm_alloc_pages(4);
        TEST_ASSERT_NEQ(pages, PMM_ALLOC_FAILED, "alloc 4 contiguous");
        TEST_ASSERT(PAGE_ALIGNED(pages), "contiguous base aligned");
        pmm_free_pages(pages, 4);
    }

    // ── Aligned allocation ──────────────────────────────
    {
        uint64_t pages = pmm_alloc_aligned(2, 2);
        TEST_ASSERT_NEQ(pages, PMM_ALLOC_FAILED, "alloc 2 aligned");
        TEST_ASSERT(PAGE_ALIGNED(pages), "aligned base page-aligned");
        TEST_ASSERT_EQ(ADDR_TO_PFN(pages) % 2, 0,
                       "PFN aligned to 2");
        pmm_free_pages(pages, 2);

        pages = pmm_alloc_aligned(4, 4);
        TEST_ASSERT_NEQ(pages, PMM_ALLOC_FAILED, "alloc 4 aligned-4");
        TEST_ASSERT_EQ(ADDR_TO_PFN(pages) % 4, 0,
                       "PFN aligned to 4");
        pmm_free_pages(pages, 4);
    }

    // ── Zero-filled allocation ──────────────────────────
    {
        uint64_t page = pmm_alloc_page_zero();
        TEST_ASSERT_NEQ(page, PMM_ALLOC_FAILED, "alloc zeroed page");

        uint8_t *ptr = (uint8_t *)PHYS_TO_VIRT(page);
        bool clean = true;
        for (size_t i = 0; i < PAGE_SIZE; ++i)
        {
            if (ptr[i] != 0) { clean = false; break; }
        }
        TEST_ASSERT(clean, "zeroed page is clean");
        pmm_free_page(page);
    }

    // ── mark_used / mark_free / is_free ─────────────────
    {
        uint64_t page = pmm_alloc_page();
        pmm_free_page(page);
        TEST_ASSERT(pmm_is_free(page), "freed page is free");

        pmm_mark_used(page, page + PAGE_SIZE);
        TEST_ASSERT(!pmm_is_free(page), "marked-used page is not free");

        pmm_mark_free(page, page + PAGE_SIZE);
        TEST_ASSERT(pmm_is_free(page), "marked-free page is free");
    }

    // ── Stats consistency ───────────────────────────────
    {
        struct pmm_stats s1, s2;
        pmm_get_stats(&s1);

        uint64_t page = pmm_alloc_page();
        pmm_get_stats(&s2);
        TEST_ASSERT_EQ(s2.free_pages, s1.free_pages - 1,
                       "alloc decrements free count");
        TEST_ASSERT_EQ(s2.reserved_pages, s1.reserved_pages + 1,
                       "alloc increments reserved count");
        pmm_free_page(page);

        pmm_get_stats(&s2);
        TEST_ASSERT_EQ(s2.free_pages, s1.free_pages,
                       "free restores count");
    }

    // ── Alloc/free stress: allocate many, free all ──────
    {
        uint64_t pages[TEST_PMM_MAX_PAGES];
        size_t count = 0;

        for (size_t i = 0; i < TEST_PMM_MAX_PAGES; ++i)
        {
            pages[i] = pmm_alloc_page();
            if (pages[i] == PMM_ALLOC_FAILED)
                break;
            count++;
        }
        TEST_ASSERT_EQ(count, TEST_PMM_MAX_PAGES,
                       "allocated all stress pages");

        // Every page should be distinct
        bool distinct = true;
        for (size_t i = 0; i < count && distinct; ++i)
            for (size_t j = i + 1; j < count && distinct; ++j)
                if (pages[i] == pages[j])
                    distinct = false;
        TEST_ASSERT(distinct, "all pages are distinct");

        for (size_t i = 0; i < count; ++i)
            pmm_free_page(pages[i]);
    }

    // ── Reuse after free ────────────────────────────────
    {
        uint64_t a = pmm_alloc_page();
        pmm_free_page(a);
        uint64_t b = pmm_alloc_page();
        TEST_ASSERT_NEQ(b, PMM_ALLOC_FAILED,
                        "alloc after free succeeds");
        pmm_free_page(b);
    }

    TEST_END();
}

