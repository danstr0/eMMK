/**
 * @file pmm.h
 * @brief Physical Memory Manager (PMM) interface.
 *
 * Manages physical page frames using a bitmap allocator. Each bit in the
 * bitmap represents one 4 KiB page frame: 0 = free, 1 = allocated/reserved.
 *
 * @par Initialization
 * @c pmm_init() is called from @c kernel_main() with the Multiboot2 memory
 * map. It records the RAM regions, places the bitmap in free RAM below
 * 1 GiB, reserves the low 1 MiB, the kernel image, the Multiboot2 info
 * structure and the bitmap, and marks all other available frames free.
 *
 * Allocation functions return physical addresses (@c uint64_t). Use
 * @c PHYS_TO_VIRT() to obtain a usable pointer through the direct map
 * (after @c vmm_init()), or @c pmm_phys_to_virt() at any boot stage.
 *
 * @par Error convention
 * Allocation functions return @c PMM_ALLOC_FAILED (0) on failure.
 * Physical address 0 is never a valid allocation target.
 */

#ifndef EMMK_PMM_H
#define EMMK_PMM_H

#include "kernel.h"

/// Allocation sentinel - returned on failure. Physical address 0 is
/// never allocated, so 0 is an unambiguous error value.
#define PMM_ALLOC_FAILED  0

/**
 * Highest physical address the PMM manages (1 TiB). Bounds the bitmap
 * (32 MiB at the limit), which must fit in free RAM below 1 GiB.
 */
#define PMM_MAX_MEMORY    (1UL << 40)

/// Maximum number of memory-map regions recorded for the direct map.
#define PMM_MAX_REGIONS   64

/// Snapshot of PMM state for diagnostics.
struct pmm_stats
{
    uint64_t total_pages;        ///< Total managed page frames.
    uint64_t free_pages;         ///< Currently free page frames.
    uint64_t reserved_pages;     ///< Allocated / reserved frames.
    uint64_t total_memory;       ///< Total managed memory (bytes).
    uint64_t free_memory;        ///< Free memory (bytes).
    uint64_t max_physical_addr;  ///< One past the highest managed address.
};

/**
 * @brief A physical region that belongs in the direct map.
 *
 * Covers memory-map entries of type available, ACPI reclaimable and
 * ACPI NVS: all ordinary RAM, safe to map write-back. Device memory
 * (MMIO) never appears here.
 */
struct pmm_region
{
    uint64_t start;  ///< First byte (physical).
    uint64_t end;    ///< One past the last bye (physical).
    uint32_t type;   ///< @c MULTIBOOT2_MEMORY_* type.
};

/// @name Initialization
/// @{

struct multiboot2_tag_mmap;

/**
 * @brief Initialize the physical memory manager.
 *
 * @param mmap       Multiboot2 memory map tag.
 * @param mbi_start  Physical address of the Multiboot2 info structure.
 * @param mbi_end    One past its last byte (physical).
 *
 * @b Must be called before any allocation function. Until
 * @c pmm_enable_direct_map() is called, the PMM accesses its bitmap and
 * zeroes pages through the boot mapping (first 1 GiB only).
 */
void pmm_init(struct multiboot2_tag_mmap *mmap,
              uint64_t mbi_start, uint64_t mbi_end);

/**
 * @brief Switch the PMM to the direct map.
 *
 * Called once by @c vmm_init() immediately after loading the kernel's
 * own page tables. From then on @c pmm_phys_to_virt() is equivalent to
 * @c PHYS_TO_VIRT() and any frame can be zeroed.
 */
void pmm_enable_direct_map(void);

/**
 * @brief Translate a physical address at any boot stage.
 *
 * Before the direct map exists, only addresses below @c BOOT_MAP_LIMIT
 * are reachable (via @c BOOT_PHYS_TO_VIRT); after, any RAM address.
 */
void *pmm_phys_to_virt(uint64_t paddr);
/// @}

/// @name Page allocation
/// @{

/**
 * @brief Allocate a single physical page frame.
 * @return Physical address of the allocated frame,
 *         or @c PMM_ALLOC_FAILED if no memory is available.
 */
uint64_t pmm_alloc_page(void);

/**
 * @brief Allocate @p count contiguous physical page frames.
 * @param count  Number of contiguous frames required.
 * @return Physical address of the first frame,
 *         or @c PMM_ALLOC_FAILED if a large enough block is unavailable.
 */
uint64_t pmm_alloc_pages(size_t count);

/**
 * @brief Allocate @p count contiguous, naturally-aligned page frames.
 *
 * The returned physical address is aligned to `align_pages * PAGE_SIZE`.
 * @c pmm_alloc_aligned(512, 512) yields one 2 MiB-aligned 2 MiB block,
 * suitable for a large-page mapping.
 *
 * @param count        Number of contiguous frames required.
 * @param align_pages  Alignment in pages (must be a power of two).
 * @return Physical address of the first frame, or @c PMM_ALLOC_FAILED.
 */
uint64_t pmm_alloc_aligned(size_t count, size_t align_pages);

/**
 * @brief Allocate a single page frame and zero its contents.
 *
 * Before @c pmm_enable_direct_map(), fails (returns @c PMM_ALLOC_FAILED)
 * if the only free frame lies above the boot mapping.
 *
 * @return Physical address of the zeroed frame,
 *         or @c PMM_ALLOC_FAILED.
 */
uint64_t pmm_alloc_page_zero(void);
/// @}

/// @name Page deallocation
/// @{

/**
 * @brief Free a single previously-allocated page frame.
 * @param paddr  Physical address of the frame to free (must be page-aligned).
 */
void pmm_free_page(uint64_t paddr);

/**
 * @brief Free @p count contiguous previously-allocated page frames.
 * @param paddr  Physical address of the first frame (must be page-aligned).
 * @param count  Number of contiguous frames to free.
 */
void pmm_free_pages(uint64_t paddr, size_t count);
/// @}

/// @name Diagnostics
/// @{

/**
 * @brief Fill @p stats with current PMM state.
 * @pre @p stats is caller-allocated.
 */
void pmm_get_stats(struct pmm_stats *stats);
/// @}

/// @name Memory regions
/// @{

/// @return Number of recorded RAM regions (see `struct pmm_region`).
size_t pmm_region_count(void);

/// @return Region @p index, or @c NULL if out of range.
const struct pmm_region *pmm_get_region(size_t index);
/// @}

/// @name Bitmap range manipulation
/// @{

/**
 * @brief Mark a physical address range as allocated/reserved.
 *
 * @param start  First byte of the range (rounded down to page boundary).
 * @param end    One past the last byte (exclusive, rounded up).
 *
 * Used to reserve regions that must never be allocated.
 */
void pmm_mark_used(uint64_t start, uint64_t end);

/**
 * @brief Mark a physical address range as available.
 *
 * @param start  First byte of the range (rounded up to page boundary).
 * @param end    One past the last byte (exclusive, rounded down).
 */
void pmm_mark_free(uint64_t start, uint64_t end);

/**
 * @brief Check whether a single page frame is free.
 * @param paddr  Physical address within the frame to test.
 * @return @c true if the frame is free, @c false otherwise.
 */
bool pmm_is_free(uint64_t paddr);
/// @}

#endif // EMMK_PMM_H

