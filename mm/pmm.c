/**
 * @file pmm.c
 * @brief Physical memory manager (bitmap allocator).
 *
 * Uses a single bitmap where each bit represents one 4 KiB page frame:
 * - bit = 1 → allocated / reserved
 * - bit = 0 → free
 *
 * @par Initialization strategy
 * 1. Scan the Multiboot2 memory map to find the highest usable address.
 * 2. Place the bitmap immediately after the kernel image in physical memory.
 * 3. Mark all pages as used (all bits set).
 * 4. Clear bits for regions the memory map reports as available.
 * 5. Re-mark the kernel image, bitmap, and low memory as reserved.
 *
 * Allocation uses a rotor (@c pmm_last_alloc) to avoid always scanning
 * from the beginning, providing roughly O(n/64) amortised allocation in 
 * the common case.
 */

#include "pmm.h"
#include "multiboot2.h"
#include "serial.h"
#include "string.h"

// ── Linker symbols & configuration ─────────────────────────────────────

/**
 * Maximum physical memory to manage (bytes).
 * 
 * Limited to 1 GiB to match the identity/higher-half mapping established
 * in multiboot2.asm. Increase this when the VMM supports additional page
 * table mappings.
 */
#define PMM_MAX_MEMORY  (1ULL * 1024 * 1024 * 1024)

/**
 * @c _kernel_physical_end is defined in the linker script as a raw
 * physical value. When accessed from C, the compiler treats the symbol's
 * "address" as its value, so @c (uintptr_t)_kernel_physical_end yields
 * the physical address numerically — no @c VIRT_TO_PHYS conversion needed.
 */
extern char _kernel_physical_end[];

// ── Forward declarations ───────────────────────────────────────────────

void pmm_free_pages(uint64_t paddr, size_t count);

// ── Bitmap state ───────────────────────────────────────────────────────

/// Pointer to the bitmap (virtual address, within the higher-half map).
static uint64_t *pmm_bitmap;

/// Size of the bitmap in bytes (page-aligned).
static uint64_t pmm_bitmap_size;

/// Physical address of the bitmap.
static uint64_t pmm_bitmap_phys;

/// Total number of page frames managed by the PMM.
static uint64_t pmm_total_pages;

/// Number of currently free page frames.
static uint64_t pmm_free_count;

/// PFN of the last successful allocation (rotor for fast scanning).
static uint64_t pmm_last_alloc;

// ── Bitmap helpers ─────────────────────────────────────────────────────

#define BITS_PER_WORD    64
#define WORD_INDEX(pfn)  ((pfn) / BITS_PER_WORD)
#define BIT_INDEX(pfn)   ((pfn) % BITS_PER_WORD)

/// Number of 64-bit words needed to cover all managed pages.
#define BITMAP_NUM_WORDS  ((pmm_total_pages + BITS_PER_WORD - 1) / BITS_PER_WORD)

/// Mark a page frame as allocated (set bit).
static inline void bitmap_set(uint64_t pfn)
{
    pmm_bitmap[WORD_INDEX(pfn)] |= (1ULL << BIT_INDEX(pfn));
}

/// Mark a page frame as free (clear bit).
static inline void bitmap_clear(uint64_t pfn)
{
    pmm_bitmap[WORD_INDEX(pfn)] &= ~(1ULL << BIT_INDEX(pfn));
}

/// Test whether a page frame is allocated (bit is set).
static inline bool bitmap_test(uint64_t pfn)
{
    return (pmm_bitmap[WORD_INDEX(pfn)] & (1ULL << BIT_INDEX(pfn))) != 0;
}

// ── Initialization ─────────────────────────────────────────────────────

void pmm_init(struct multiboot2_tag_mmap *mmap, uint64_t mb_info_end)
{
    struct multiboot2_mmap_entry *entry;
    uint64_t max_addr = 0;
    uint64_t kernel_end_phys;

    // 1. Find the highest usable physical address
    multiboot2_mmap_foreach(mmap, entry)
    {
        if (entry->type == MULTIBOOT2_MEMORY_AVAILABLE)
        {
            uint64_t end = entry->addr + entry->len;
            if (end > max_addr)
                max_addr = end;
        }
    }

    if (max_addr > PMM_MAX_MEMORY)
        max_addr = PMM_MAX_MEMORY;

    // 2. Calculate bitmap dimensions
    pmm_total_pages = max_addr / PAGE_SIZE;
    pmm_bitmap_size = PAGE_ALIGN_UP((pmm_total_pages + 7) / 8);

    // 3. Place bitmap immediately after mb_info or the kernel image
    kernel_end_phys = PAGE_ALIGN_UP((uintptr_t)_kernel_physical_end);
    uint64_t bitmap_start = kernel_end_phys;
    if (mb_info_end > bitmap_start)
        bitmap_start = PAGE_ALIGN_UP(mb_info_end);
    pmm_bitmap = (uint64_t *)PHYS_TO_VIRT(bitmap_start);

    // 4. Mark every page as used (all bits set)
    memset(pmm_bitmap, 0xFF, pmm_bitmap_size);
    pmm_free_count = 0;

    // 5. Free pages in regions the memory map reports as available
    multiboot2_mmap_foreach(mmap, entry)
    {
        if (entry->type == MULTIBOOT2_MEMORY_AVAILABLE)
        {
            uint64_t start = PAGE_ALIGN_UP(entry->addr);
            uint64_t end   = PAGE_ALIGN_DOWN(entry->addr + entry->len);

            if (start >= PMM_MAX_MEMORY)
                continue;
            if (end > PMM_MAX_MEMORY)
                end = PMM_MAX_MEMORY;

            for (uint64_t addr = start; addr < end; addr += PAGE_SIZE)
            {
                bitmap_clear(ADDR_TO_PFN(addr));
                pmm_free_count++;
            }
        }
    }

    // 6. Re-reserve vital regions that overlap "available" memory:
    //      - Low 1 MiB: real-mode IVT, BDA, EBDA, BIOS ROM
    //      - Kernel image + bitmap: must not be allocated over
    pmm_bitmap_phys = bitmap_start;

    pmm_mark_used(0, 0x100000);
    pmm_mark_used(0x100000, pmm_bitmap_phys + pmm_bitmap_size);

    pmm_last_alloc = 0;

    kprintf("[PMM] Bitmap at phys 0x%lx, %lu bytes (%lu words)\n",
            kernel_end_phys, pmm_bitmap_size, BITMAP_NUM_WORDS);
    kprintf("[PMM] %lu / %lu pages free (%lu MiB / %lu MiB)\n",
            pmm_free_count, pmm_total_pages,
            (pmm_free_count * PAGE_SIZE) / (1024 * 1024),
            (pmm_total_pages * PAGE_SIZE) / (1024 * 1024));
}

// ── Single page allocation ─────────────────────────────────────────────

uint64_t pmm_alloc_page(void)
{
    uint64_t num_words = BITMAP_NUM_WORDS;
    uint64_t start_word = WORD_INDEX(pmm_last_alloc);

    for (uint64_t i = 0; i < num_words; ++i)
    {
        uint64_t idx = (start_word + i) % num_words;
        uint64_t word = pmm_bitmap[idx];

        // Skip fully-allocated words
        if (word == ~0ULL)
            continue;

        // Find the first free bit in this word
        int bit = __builtin_ctzll(~word);
        uint64_t pfn = (idx * BITS_PER_WORD) + bit;

        if (pfn < pmm_total_pages)
        {
            bitmap_set(pfn);
            pmm_free_count--;
            pmm_last_alloc = pfn;
            return PFN_TO_ADDR(pfn);
        }
    }

    kprintf("[PMM] WARNING: Out of memory!\n");
    return PMM_ALLOC_FAILED;
}

// ── Contiguous page allocation ─────────────────────────────────────────

uint64_t pmm_alloc_pages(size_t count)
{
    if (count == 0)
        return PMM_ALLOC_FAILED;
    if (count == 1)
        return pmm_alloc_page();

    // Linear scan for a contiguous run of free frames
    // Starts from the rotor position and wraps around once
    uint64_t run_start = 0;
    uint64_t run_len = 0;
    uint64_t start_pfn = pmm_last_alloc;

    for (uint64_t i = 0; i < pmm_total_pages; ++i)
    {
        uint64_t pfn = (start_pfn + i) % pmm_total_pages;

        // Reset the run if the bitmap boundary is wrapped around
        if (pfn == 0 && i != 0)
            run_len = 0;

        if (!bitmap_test(pfn))
        {
            if (run_len == 0)
                run_start = pfn;
            run_len++;

            if (run_len == count)
            {
                for (uint64_t j = 0; j < count; ++j)
                    bitmap_set(run_start + j);
                pmm_free_count -= count;
                pmm_last_alloc = run_start + count - 1;
                return PFN_TO_ADDR(run_start);
            }
        }
        else
            run_len = 0;
    }

    kprintf("[PMM] WARNING: Failed to allocate %lu contiguous pages\n",
            (uint64_t)count);
    return PMM_ALLOC_FAILED;
}

// ── Aligned allocation ─────────────────────────────────────────────────

uint64_t pmm_alloc_aligned(size_t count, size_t align_pages)
{
    if (count == 0 || align_pages == 0)
        return PMM_ALLOC_FAILED;

    if (align_pages & (align_pages - 1))
        return PMM_ALLOC_FAILED;

    // Step through PFNs at alignment boundaries
    for (uint64_t pfn = 0; pfn + count <= pmm_total_pages; pfn += align_pages)
    {
        bool usable = true;
        for (size_t j = 0; j < count; ++j)
            if (bitmap_test(pfn + j))
            {
                usable = false;
                break;
            }

        if (usable)
        {
            for (size_t j = 0; j < count; ++j)
                bitmap_set(pfn + j);
            pmm_free_count -= count;
            pmm_last_alloc = pfn + count - 1;
            return PFN_TO_ADDR(pfn);
        }
    }

    kprintf("[PMM] WARNING: Failed to allocate %lu aligned pages"
            " (align=%lu)\n", (uint64_t)count, (uint64_t)align_pages);

    return PMM_ALLOC_FAILED;
}

// ── Zero-initialized allocation ────────────────────────────────────────

uint64_t pmm_alloc_page_zero(void)
{
    uint64_t paddr = pmm_alloc_page();
    if (paddr != PMM_ALLOC_FAILED)
        memset(PHYS_TO_VIRT(paddr), 0, PAGE_SIZE);
    return paddr;
}

// ── Deallocation ───────────────────────────────────────────────────────

void pmm_free_page(uint64_t paddr)
{
    pmm_free_pages(paddr, 1);
}

void pmm_free_pages(uint64_t paddr, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        uint64_t pfn = ADDR_TO_PFN(paddr + (i * PAGE_SIZE));

        if (pfn >= pmm_total_pages)
        {
            kprintf("[PMM] WARNING: free of out-of-range PFN %lu\n", pfn);
            continue;
        }

        if (!bitmap_test(pfn))
        {
            kprintf("[PMM] WARNING: double free of PFN %lu (phys 0x%lx)\n",
                    pfn, PFN_TO_ADDR(pfn));
            continue;
        }

        bitmap_clear(pfn);
        pmm_free_count++;
    }
}

// ── Bitmap range manipulation ──────────────────────────────────────────

void pmm_mark_used(uint64_t start, uint64_t end)
{
    for (uint64_t addr = PAGE_ALIGN_DOWN(start);
         addr < PAGE_ALIGN_UP(end);
         addr += PAGE_SIZE)
    {
        uint64_t pfn = ADDR_TO_PFN(addr);
        if (pfn < pmm_total_pages && !bitmap_test(pfn))
        {
            bitmap_set(pfn);
            pmm_free_count--;
        }
    }
}

void pmm_mark_free(uint64_t start, uint64_t end)
{
    for (uint64_t addr = PAGE_ALIGN_UP(start);
         addr < PAGE_ALIGN_DOWN(end);
         addr += PAGE_SIZE)
    {
        uint64_t pfn = ADDR_TO_PFN(addr);
        if (pfn < pmm_total_pages && bitmap_test(pfn))
        {
            bitmap_clear(pfn);
            pmm_free_count++;
        }
    }
}

// ── Queries ────────────────────────────────────────────────────────────

bool pmm_is_free(uint64_t paddr)
{
    uint64_t pfn = ADDR_TO_PFN(paddr);
    if (pfn >= pmm_total_pages)
        return false;
    return !bitmap_test(pfn);

}

void pmm_get_stats(struct pmm_stats *stats)
{
    if (!stats)
        return;

    stats->total_pages    = pmm_total_pages;
    stats->free_pages     = pmm_free_count;
    stats->reserved_pages = pmm_total_pages - pmm_free_count;
    stats->total_memory   = pmm_total_pages * PAGE_SIZE;
    stats->free_memory    = pmm_free_count * PAGE_SIZE;
    stats->max_physical_addr = PFN_TO_ADDR(pmm_total_pages);
}

