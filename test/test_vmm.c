/**
 * @file test_vmm.c
 * @brief Tests for the virtual memory manager.
 *
 * Scratch mappings use a virtual range in PML4[511] / PDPT[511], which
 * nothing else in the kernel maps. All mappings are cleaned up at the
 * end of each test.
 */

#include "test.h"
#include "pmm.h"
#include "vmm.h"

/// Test virtual address base: PML4[511], PDPT[511], PD[256] (2 MiB aligned).
#define TEST_VMM_VADDR  (0xFFFFFFFFFC000000UL + 256UL * PAGE_SIZE_2M)

// Linker symbols
extern char __boot_stack_start[];  // physical
extern char __text_start[], __rodata_start[], __data_start[];

uint32_t test_vmm(void)
{
    TEST_BEGIN("vmm");

    // ── Kernel image mapping ────────────────────────────
    {
        TEST_ASSERT(vmm_is_mapped(0xFFFFFFFF80100000UL),
                    "kernel image start is mapped");
        uint64_t phys = vmm_get_physical(0xFFFFFFFF80100000UL);
        TEST_ASSERT_EQ(phys, 0x100000,
                       "kernel image maps to phys 1MiB");
    
        uint64_t text = vmm_get_flags((uint64_t)__text_start);
        TEST_ASSERT((text & PTE_PRESENT) && !(text & PTE_WRITABLE)
                    && !(text & PTE_NO_EXECUTE), ".text is RX");
    
        uint64_t ro = vmm_get_flags((uint64_t)__rodata_start);
        TEST_ASSERT((ro & PTE_PRESENT) && !(ro & PTE_WRITABLE)
                    && (ro & PTE_NO_EXECUTE), ".rodata is RO+NX");

        uint64_t data = vmm_get_flags((uint64_t)__data_start);
        TEST_ASSERT((data & PTE_WRITABLE) && (data & PTE_NO_EXECUTE),
                    ".data is RW+NX");
    }

    // ── Guard pages ─────────────────────────────────────
    {
        TEST_ASSERT(!vmm_is_mapped(0), "NULL page unmapped");

        uint64_t guard = KERNEL_VIRTUAL_BASE
                       + (uint64_t)__boot_stack_start - PAGE_SIZE;
        TEST_ASSERT(!vmm_is_mapped(guard), "page below boot stack unmapped");
        TEST_ASSERT(vmm_is_mapped(guard + PAGE_SIZE), "boot stack mapped");
    }

    // ── Direct map ──────────────────────────────────────
    {
        TEST_ASSERT(pmm_region_count() >= 1, "RAM regions recorded");
    
        uint64_t paddr = pmm_alloc_page();
        volatile uint64_t *p = (volatile uint64_t *)PHYS_TO_VIRT(paddr);
        TEST_ASSERT_EQ(vmm_get_physical((uint64_t)p), paddr,
                       "direct map translates back");
        TEST_ASSERT_EQ(VIRT_TO_PHYS_ADDR(p), paddr,
                       "VIRT_TO_PHYS on direct map");
        *p = 0x1122334455667788UL;
        TEST_ASSERT_EQ(*p, 0x1122334455667788UL, "write through direct map");
        pmm_free_page(paddr);

        // 4 MiB is RAM in every configuration; it should sit in a large page
        uint64_t f = vmm_get_flags(DIRECT_MAP_BASE + 4 * 1024 * 1024);
        TEST_ASSERT(f & PTE_HUGE, "direct map uses large pages");
        TEST_ASSERT(f & PTE_NO_EXECUTE, "direct map is NX");
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
        TEST_ASSERT_EQ(virt_to_phys((void *)vaddr), paddr,
                       "virt_to_phys walks for non-direct addresses");

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

    // ── map_range picks a 2 MiB page ────────────────────
    {
        uint64_t paddr = pmm_alloc_aligned(512, 512);
        TEST_ASSERT_NEQ(paddr, PMM_ALLOC_FAILED, "alloc 2 MiB block");

        uint64_t vaddr = TEST_VMM_VADDR + PAGE_SIZE_2M;
        int ret = vmm_map_range(vaddr, paddr, PAGE_SIZE_2M, PTE_KERNEL_RW);
        TEST_ASSERT_EQ(ret, VMM_OK, "map_range succeeds");
        TEST_ASSERT(vmm_get_flags(vaddr) & PTE_HUGE, "mapped as a 2 MiB page");
        TEST_ASSERT_EQ(vmm_get_physical(vaddr + 0x12345), paddr + 0x12345,
                       "2 MiB page offset translation");
        
        TEST_ASSERT_EQ(vmm_map_page(vaddr + PAGE_SIZE, paddr, PTE_KERNEL_RW),
                       VMM_ERR_INVALID, "4K map inside a 2M page rejected");
        TEST_ASSERT_EQ(vmm_unmap_page(vaddr + PAGE_SIZE), 0,
                       "partial unmap of a 2M page rejected");

        TEST_ASSERT_EQ(vmm_unmap_page(vaddr), paddr, "unmap a whole 2M page");
        TEST_ASSERT(!vmm_is_mapped(vaddr), "2M page unmapped");
        pmm_free_pages(paddr, 512);
    }

    // ── MMIO mappings ───────────────────────────────────
    {
        uint64_t paddr = pmm_alloc_page();
        volatile uint8_t *a = vmm_map_mmio(paddr + 0x10, 8);
        volatile uint8_t *b = vmm_map_mmio(paddr, PAGE_SIZE);
    
        TEST_ASSERT(a != NULL && b != NULL, "map_mmio succeeds");
        TEST_ASSERT((uint64_t)a >= MMIO_VIRTUAL_BASE, "MMIO in its window");
        TEST_ASSERT_EQ((uint64_t)a & 0xFFF, 0x10, "MMIO keeps page offset");
        TEST_ASSERT((uint64_t)a - 0x10 != (uint64_t)b,
                    "each MMIO call gets its own mapping");
    
        uint64_t f = vmm_get_flags((uint64_t)a);
        TEST_ASSERT((f & PTE_CACHE_DISABLE) && (f & PTE_WRITE_THROUGH),
                    "MMIO mapped uncached (PCD|PWT)");
    
        b[0x10] = 0x5A;
        TEST_ASSERT_EQ(a[0], 0x5A, "two MMIO views of one frame agree");
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
        TEST_ASSERT(!(vmm_get_flags(vaddr) & PTE_WRITABLE),
                    "set_flags cleared WRITABLE");

        vmm_unmap_page(vaddr);
        pmm_free_page(paddr);
    }

    TEST_END();
}

