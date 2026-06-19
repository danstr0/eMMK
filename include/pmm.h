/**
 * @file pmm.h
 * @brief Physical Memory Manager (PMM) interface.
 *
 * Manages physical page frames using a bitmap allocator. Each bit in the
 * bitmap represents one 4 KiB page frame: 0 = free, 1 = allocated/reserved.
 *
 * @par Initialization
 * @c pmm_init() is called from @c kernel_main() with the Multiboot2 memory
 * map. It scans available regions, reserves the kernel and bitmap 
 * themselves, and marks all other available frames as free.
 *
 * Allocation functions return physical addresses (@c uint64_t). Use 
 * @c PHYS_TO_VIRT() to obtain a usable virtual pointer within the 
 * higher-half direct mapping.
 *
 * @par Error convention
 * Allocation functions return @c PMM_ALLOC_FAILED (0) on failure. Physical
 * address 0 is never a valid allocation target.
 */

#ifndef EMMK_PMM_H
#define EMMK_PMM_H

#include "kernel.h"

/// Allocation sentinel - returned on failure. Physical address 0 is
/// never allocated, so 0 is an unambiguous error value.
#define PMM_ALLOC_FAILED  0

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

/// @name Initialization
/// @{

/**
 * @brief Initialize the physical memory manager.
 *
 * Scans the Multiboot2 memory map to determine available physical regions,
 * places the bitmap immediately after multiboot info or the kernel
 * image, and marks the kernel image, multiboot info, and bitmap as reserved.
 * @b Must be called before any allocation functions.
 *
 * @param mmap  Multiboot2 memory map tag.
 */
struct multiboot2_tag_mmap;
void pmm_init(struct multiboot2_tag_mmap *mmap, uint64_t mb_info_end);
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
 * The returned physical address is aligned to @p align_pages * @c PAGE_SIZE.
 *
 * @param count        Number of contiguous frames required.
 * @param align_pages  Alignment in pages (must be a power of two).
 * @return Physical address of the first frame, or @c PMM_ALLOC_FAILED.
 */
uint64_t pmm_alloc_aligned(size_t count, size_t align_pages);

/**
 * @brief Allocate a single page frame and zero its contents.
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

/// @name Bitmap range manipulation
/// @{

/**
 * @brief Mark a physical address range as allocated/reserved.
 *
 * @param start  First byte of the range (rounded down to page boundary).
 * @param end    One past the last byte (exclusive, rounded up).
 *
 * Used during init to reserve the kernel image, Multiboot2 structures,
 * and other regions that must not be allocated.
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

