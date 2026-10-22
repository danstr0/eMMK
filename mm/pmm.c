/**
 * @file pmm.c
 * @brief Physical memory manager (bitmap allocator).
 *
 * Uses a single bitmap where each bit represents one 4 KiB page frame:
 * - bit = 1 → allocated / reserved
 * - bit = 0 → free
 *
 * @par Initialization strategy
 * 1. Record the RAM regions of the Multiboot2 memory map (for the VMM's
 *    direct map) and find the highest available address.
 * 2. Place the bitmap in free RAM below 1 GiB (reachable through the boot
 *    page tables), avoiding the kernel image and the Multiboot2 info.
 * 3. Mark all pages as used (all bits set).
 * 4. Clear bits for regions the memory map reports as available.
 * 5. Re-reserve the low 1 MiB, the kernel image, the Multiboot2 info, and
 *    the bitmap.
 *
 * Allocation uses a rotor (@c pmm_last_alloc) to avoid always scanning
 * from the beginning, providing roughly O(n/64) amortised allocation in
 * the common case.
 *
 * All bitmap state is protected by @c pmm_lock.
 */

#include "pmm.h"
#include "multiboot2.h"
#include "panic.h"
#include "serial.h"
#include "string.h"
#include "spinlock.h"

// ── Linker symbols ─────────────────────────────────────────────────────

/**
 * The linker script defines these as raw physical values. When accessed
 * from C, the symbol's "address" is its value, so
 * @c (uintptr_t)_kernel_physical_end yields the physical address
 * numerically.
 */
extern char _kernel_physical_start[];
extern char _kernel_physical_end[];

// ── Bitmap state ───────────────────────────────────────────────────────

/// Pointer to the bitmap (boot alias until the direct map is enabled).
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

/// Set by @c pmm_enable_direct_map().
static bool pmm_direct_map;

/// RAM regions recorded from the memory map (see `struct pmm_region`).
static struct pmm_region pmm_regions[PMM_MAX_REGIONS];
static size_t            pmm_num_regions;

/**
 * Protects all bitmap state (bitmap, free_count, last_alloc).
 *
 * Contention is low: the PMM is called during init, heap expansion, and
 * slab growth. The per-packet allocation paths go through the slab
 * layer's per-core magazines and never reach the PMM in steady state.
 */
static spinlock_t pmm_lock = SPINLOCK_INIT;

// ── Bitmap helpers ─────────────────────────────────────────────────────

#define BITS_PER_WORD    64
#define WORD_INDEX(pfn)  ((pfn) / BITS_PER_WORD)
#define BIT_INDEX(pfn)   ((pfn) % BITS_PER_WORD)

/// Number of 64-bit words needed to cover all managed pages.
#define BITMAP_NUM_WORDS  ((pmm_total_pages + BITS_PER_WORD - 1) / BITS_PER_WORD)

/// Mark a page frame as allocated (set bit).
static inline void bitmap_set(uint64_t pfn)
{
    pmm_bitmap[WORD_INDEX(pfn)] |= (1UL << BIT_INDEX(pfn));
}

/// Mark a page frame as free (clear bit).
static inline void bitmap_clear(uint64_t pfn)
{
    pmm_bitmap[WORD_INDEX(pfn)] &= ~(1UL << BIT_INDEX(pfn));
}

/// Test whether a page frame is allocated (bit is set).
static inline bool bitmap_test(uint64_t pfn)
{
    return (pmm_bitmap[WORD_INDEX(pfn)] & (1UL << BIT_INDEX(pfn))) != 0;
}

/// Mark a range used. Caller holds @c pmm_lock (or is single-threaded init).
static void mark_used_locked(uint64_t start, uint64_t end)
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

// ── Initialization helpers ─────────────────────────────────────────────

/// True if [a0, a1) and [b0, b1) overlap.
static inline bool ranges_overlap(uint64_t a0, uint64_t a1,
                                  uint64_t b0, uint64_t b1)
{
    return a0 < b1 && b0 < a1;
}

/**
 * @brief Find a home for the bitmap.
 *
 * The bitmap must lie entirely inside an available region, below the
 * boot mapping limit (it is written before the direct map exists), and
 * must not overlap the kernel image or the Multiboot2 info structure.
 *
 * @return Physical start address, or 0 if no region fits.
 */
static uint64_t place_bitmap(struct multiboot2_tag_mmap *mmap, uint64_t size,
                             uint64_t kern_start, uint64_t kern_end,
                             uint64_t mbi_start, uint64_t mbi_end)
{
    struct multiboot2_mmap_entry *entry;
    multiboot2_mmap_foreach(mmap, entry)
    {
        if (entry->type != MULTIBOOT2_MEMORY_AVAILABLE)
            continue;

        uint64_t start = PAGE_ALIGN_UP(MAX(entry->addr, 0x100000UL));
        uint64_t end   = PAGE_ALIGN_DOWN(MIN(entry->addr + entry->len,
                                             BOOT_MAP_LIMIT));

        // Slide past the kernel and the MBI until the candidate overlaps
        // neither
        for (int pass = 0; pass < 3; ++pass)
        {
            if (ranges_overlap(start, start + size, kern_start, kern_end))
                start = PAGE_ALIGN_UP(kern_end);
            if (ranges_overlap(start, start + size, mbi_start, mbi_end))
                start = PAGE_ALIGN_UP(mbi_end);
        }

        if (start + size <= end)
            return start;
    }
    return 0;
}

/// Record the RAM-type memory-map entries for the direct map.
static void record_regions(struct multiboot2_tag_mmap *mmap)
{
    struct multiboot2_mmap_entry *entry;
    pmm_num_regions = 0;

    multiboot2_mmap_foreach(mmap, entry)
    {
        if (entry->type != MULTIBOOT2_MEMORY_AVAILABLE
            && entry->type != MULTIBOOT2_MEMORY_ACPI_RECLAIMABLE
            && entry->type != MULTIBOOT2_MEMORY_NVS)
            continue;

        if (entry->len == 0 || entry->addr >= PMM_MAX_MEMORY)
            continue;

        if (pmm_num_regions == PMM_MAX_REGIONS)
        {
            kprintf("[PMM] WARNING: more than %u RAM regions; "
                    "ignoring the rest\n", PMM_MAX_REGIONS);
            break;
        }

        struct pmm_region *r = &pmm_regions[pmm_num_regions++];
        r->start = entry->addr;
        r->end   = MIN(entry->addr + entry->len, PMM_MAX_MEMORY);
        r->type  = entry->type;
    }
}

// ── Initialization ─────────────────────────────────────────────────────

void pmm_init(struct multiboot2_tag_mmap *mmap,
              uint64_t mbi_start, uint64_t mbi_end)
{
    struct multiboot2_mmap_entry *entry;
    uint64_t max_addr = 0;

    uint64_t kern_start = PAGE_ALIGN_DOWN((uintptr_t)_kernel_physical_start);
    uint64_t kern_end   = PAGE_ALIGN_UP((uintptr_t)_kernel_physical_end);

    record_regions(mmap);

    // 1. Find the highest available physical address
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
    {
        kprintf("[PMM] WARNING: RAM above %lu GiB ignored\n",
                PMM_MAX_MEMORY >> 30);
        max_addr = PMM_MAX_MEMORY;
    }

    // 2. Calculate bitmap dimensions and place it
    pmm_total_pages = max_addr / PAGE_SIZE;
    pmm_bitmap_size = PAGE_ALIGN_UP((pmm_total_pages + 7) / 8);

    pmm_bitmap_phys = place_bitmap(mmap, pmm_bitmap_size,
                                   kern_start, kern_end,
                                   mbi_start, mbi_end);
    if (pmm_bitmap_phys == 0)
    {
        panic("[PMM] no room for a %lu KiB bitmap below 1 GiB",
              pmm_bitmap_size / 1024);
    }
    pmm_bitmap = (uint64_t *)BOOT_PHYS_TO_VIRT(pmm_bitmap_phys);

    // 3. Mark every page as used (all bits set)
    memset(pmm_bitmap, 0xFF, pmm_bitmap_size);
    pmm_free_count = 0;

    // 4. Free pages in regions the memory map reports as available
    multiboot2_mmap_foreach(mmap, entry)
    {
        if (entry->type == MULTIBOOT2_MEMORY_AVAILABLE)
        {
            uint64_t start = PAGE_ALIGN_UP(entry->addr);
            uint64_t end   = PAGE_ALIGN_DOWN(entry->addr + entry->len);

            if (start >= max_addr)
                continue;
            if (end > max_addr)
                end = max_addr;

            for (uint64_t addr = start; addr < end; addr += PAGE_SIZE)
            {
                uint64_t pfn = ADDR_TO_PFN(addr);
                if (bitmap_test(pfn))  // regions may overlap
                {
                    bitmap_clear(pfn);
                    pmm_free_count++;
                }
            }
        }
    }

    // 5. Re-reserve vital regions that overlap "available" memory
    //      - Low 1 MiB: real-mode IVT, BDA, EBDA, BIOS ROM, AP trampoline
    //      - Kernel image (incl. boot page tables and boot stack)
    //      - Multiboot2 info structure
    //      - The bitmap itself
    mark_used_locked(0, 0x100000);
    mark_used_locked(kern_start, kern_end);
    mark_used_locked(mbi_start, mbi_end);
    mark_used_locked(pmm_bitmap_phys, pmm_bitmap_phys + pmm_bitmap_size);

    pmm_last_alloc = 0;

    kprintf("[PMM] Bitmap at phys 0x%lx, %lu bytes (%lu words)\n",
            pmm_bitmap_phys, pmm_bitmap_size, BITMAP_NUM_WORDS);
    kprintf("[PMM] %lu / %lu pages free (%lu MiB / %lu MiB), "
            "%zu RAM regions\n",
            pmm_free_count, pmm_total_pages,
            (pmm_free_count * PAGE_SIZE) / (1024 * 1024),
            (pmm_total_pages * PAGE_SIZE) / (1024 * 1024),
            pmm_num_regions);
}

void pmm_enable_direct_map(void)
{
    pmm_bitmap     = (uint64_t *)PHYS_TO_VIRT(pmm_bitmap_phys);
    pmm_direct_map = true;
}

void *pmm_phys_to_virt(uint64_t paddr)
{
    return pmm_direct_map ? PHYS_TO_VIRT(paddr) : BOOT_PHYS_TO_VIRT(paddr);
}

// ── Single page allocation ─────────────────────────────────────────────

uint64_t pmm_alloc_page(void)
{
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    uint64_t num_words = BITMAP_NUM_WORDS;
    uint64_t start_word = WORD_INDEX(pmm_last_alloc);

    for (uint64_t i = 0; i < num_words; ++i)
    {
        uint64_t idx = (start_word + i) % num_words;
        uint64_t word = pmm_bitmap[idx];

        // Skip fully-allocated words
        if (word == ~0UL)
            continue;

        // Find the first free bit in this word
        int bit = __builtin_ctzl(~word);
        uint64_t pfn = (idx * BITS_PER_WORD) + bit;

        if (pfn < pmm_total_pages)
        {
            bitmap_set(pfn);
            pmm_free_count--;
            pmm_last_alloc = pfn;
            spin_unlock_irqrestore(&pmm_lock, flags);
            return PFN_TO_ADDR(pfn);
        }
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
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

    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

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

        // Outside a run, skip whole fully-allocated words at once
        if (run_len == 0 && BIT_INDEX(pfn) == 0
            && pfn + BITS_PER_WORD <= pmm_total_pages
            && pmm_bitmap[WORD_INDEX(pfn)]== ~0UL)
        {
            i += BITS_PER_WORD - 1;
            continue;
        }

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
                spin_unlock_irqrestore(&pmm_lock, flags);
                return PFN_TO_ADDR(run_start);
            }
        }
        else
            run_len = 0;
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
    kprintf("[PMM] WARNING: Failed to allocate %zu contiguous pages\n",
            count);
    return PMM_ALLOC_FAILED;
}

// ── Aligned allocation ─────────────────────────────────────────────────

uint64_t pmm_alloc_aligned(size_t count, size_t align_pages)
{
    if (count == 0 || align_pages == 0)
        return PMM_ALLOC_FAILED;

    if (align_pages & (align_pages - 1))
        return PMM_ALLOC_FAILED;

    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

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
            spin_unlock_irqrestore(&pmm_lock, flags);
            return PFN_TO_ADDR(pfn);
        }
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
    kprintf("[PMM] WARNING: Failed to allocate %zu aligned pages"
            " (align=%zu)\n", count, align_pages);
    return PMM_ALLOC_FAILED;
}

// ── Zero-initialized allocation ────────────────────────────────────────

uint64_t pmm_alloc_page_zero(void)
{
    uint64_t paddr = pmm_alloc_page();
    if (paddr == PMM_ALLOC_FAILED)
        return paddr;

    if (!pmm_direct_map && paddr >= BOOT_MAP_LIMIT)
    {
        // Not reachable through the boot mapping yet
        kprintf("[PMM] ERROR: early zeroed page 0x%lx above boot map\n",
                paddr);
        pmm_free_page(paddr);
        return PMM_ALLOC_FAILED;
    }

    memset(pmm_phys_to_virt(paddr), 0, PAGE_SIZE);
    return paddr;
}

// ── Deallocation ───────────────────────────────────────────────────────

void pmm_free_page(uint64_t paddr)
{
    pmm_free_pages(paddr, 1);
}

void pmm_free_pages(uint64_t paddr, size_t count)
{
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

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

    spin_unlock_irqrestore(&pmm_lock, flags);
}

// ── Bitmap range manipulation ──────────────────────────────────────────

void pmm_mark_used(uint64_t start, uint64_t end)
{
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    mark_used_locked(start, end);
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_mark_free(uint64_t start, uint64_t end)
{
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

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

    spin_unlock_irqrestore(&pmm_lock, flags);
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

size_t pmm_region_count(void)
{
    return pmm_num_regions;
}

const struct pmm_region *pmm_get_region(size_t index)
{
    return index < pmm_num_regions ? &pmm_regions[index] : NULL;
}

