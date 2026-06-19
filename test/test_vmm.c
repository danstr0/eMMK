/**
 * @file test_vmm.c
 * @brief Tests for the virtual memory manager.
 *
 * Uses a virtual address range outside the boot-mapped 1 GiB
 * (PDPT[511] space) to avoid collisions with boot huge pages.
 * All mappings are cleaned up at the end of each test.
 */

#include "test.h"
#include "pmm.h"
#include "vmm.h"

/// Test virtual address base: PDPT[511], PD[256] — well above the heap.
#define TEST_VMM_VADDR  0xFFFFFFFFC0000000ULL + (256ULL * 2 * 1024 * 1024)

uint32_t test_vmm(void)
{
    TEST_BEGIN("vmm");

    // ── Boot mappings are visible ───────────────────────
    {
        TEST_ASSERT(vmm_is_mapped(0xFFFFFFFF80100000ULL),
                    "kernel text is mapped");
        uint64_t phys = vmm_get_physical(0xFFFFFFFF80100000ULL);
        TEST_ASSERT_EQ(phys, 0x100000,
                       "kernel text maps to phys 1MiB");
    }

    // ── Map / translate / unmap a single page ───────────
    {
        uint64_t paddr = pmm_alloc_page();
        TEST_ASSERT_NEQ(paddr, PMM_ALLOC_FAILED, "alloc page for map");

        uint64_t vaddr = TEST_VMM_VADDR;
        int ret = vmm_map_page(vaddr, paddr, PTE_KERNEL_RW);
        TEST_ASSERT_EQ(ret, VMM_OK, "map_page succeeds");

        TEST_ASSERT(vmm_is_mapped(vaddr), "page is mapped");
        TEST_ASSERT_EQ(vmm_get_physical(vaddr), paddr,
                       "translation correct");

        // Write through the mapping and verify
        volatile uint64_t *ptr = (volatile uint64_t *)vaddr;
        *ptr = 0xCAFEBABE;
        TEST_ASSERT_EQ(*ptr, 0xCAFEBABE, "write through mapping");

        uint64_t unmapped = vmm_unmap_page(vaddr);
        TEST_ASSERT_EQ(unmapped, paddr, "unmap returns phys addr");
        TEST_ASSERT(!vmm_is_mapped(vaddr), "page is unmapped");

        pmm_free_page(paddr);
    }

    // ── Map / unmap multiple contiguous pages ───────────
    {
        size_t count = 4;
        uint64_t paddr = pmm_alloc_pages(count);
        TEST_ASSERT_NEQ(paddr, PMM_ALLOC_FAILED, "alloc pages for map");

        uint64_t vaddr = TEST_VMM_VADDR + (16 * PAGE_SIZE);
        int ret = vmm_map_pages(vaddr, paddr, count, PTE_KERNEL_RW);
        TEST_ASSERT_EQ(ret, VMM_OK, "map_pages succeeds");

        for (size_t i = 0; i < count; ++i)
            TEST_ASSERT(vmm_is_mapped(vaddr + i * PAGE_SIZE),
                        "multi-page: each page mapped");

        vmm_unmap_pages(vaddr, count);

        for (size_t i = 0; i < count; ++i)
            TEST_ASSERT(!vmm_is_mapped(vaddr + i * PAGE_SIZE),
                        "multi-page: each page unmapped");

        pmm_free_pages(paddr, count);
    }

    // ── Unmap of unmapped address returns 0 ─────────────
    {
        uint64_t vaddr = TEST_VMM_VADDR + (64 * PAGE_SIZE);
        uint64_t result = vmm_unmap_page(vaddr);
        TEST_ASSERT_EQ(result, 0, "unmap unmapped returns 0");
    }

    // ── get_physical on unmapped address returns 0 ──────
    {
        uint64_t vaddr = TEST_VMM_VADDR + (128 * PAGE_SIZE);
        TEST_ASSERT_EQ(vmm_get_physical(vaddr), 0,
                       "get_physical unmapped returns 0");
    }

    // ── set_flags on a mapped page ──────────────────────
    {
        uint64_t paddr = pmm_alloc_page();
        uint64_t vaddr = TEST_VMM_VADDR + (32 * PAGE_SIZE);

        vmm_map_page(vaddr, paddr, PTE_KERNEL_RW);
        int ret = vmm_set_flags(vaddr, PTE_KERNEL_RO);
        TEST_ASSERT_EQ(ret, VMM_OK, "set_flags succeeds");

        vmm_unmap_page(vaddr);
        pmm_free_page(paddr);
    }

    TEST_END();
}

