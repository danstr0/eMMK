/**
 * @file vmm.h
 * @brief Virtual Memory Manager (VMM) interface.
 *
 * Manages the kernel's 4-level (PML4) pages tables on x86-64. Provides
 * functions to map, unmap, and query virtual-to-physical translations.
 *
 * @par Page table hierarchy (4 KiB pages)
 * @code
 *   PML4 -> PDPT ->  PD  -> PT   -> Physical Page
 *   [47:39] [38:30] [29:21] [20:12] [11:0] offset
 * @endcode
 *
 * @par Address space layout
 * @code
 *   0xFFFF_FFFF_8000_0000 .. 0xFFFF_FFFF_BFFF_FFFF
 *   Higher-half kernel mapping (1 GiB, set up by boot code)
 * @endcode
 *
 * Physical-to-virtual conversions use @c KERNEL_VIRTUAL_BASE from pmm.h.
 * The VMM allocates page table frames via @c pmm_alloc_page_zero().
 */

#ifndef EMMK_VMM_H
#define EMMK_VMM_H

#include "types.h"
#include "kernel.h"

/// @name Page table entry flags
/// @{
#define PTE_PRESENT        (1UL << 0)   ///< Page is present in memory.
#define PTE_WRITABLE       (1UL << 1)   ///< Page is writable.
#define PTE_USER           (1UL << 2)   ///< Accessible from ring 3 (UNUSED).
#define PTE_WRITE_THROUGH  (1UL << 3)   ///< Write-through caching policy.
#define PTE_CACHE_DISABLE  (1UL << 4)   ///< Disable caching (for MMIO).
#define PTE_ACCESSED       (1UL << 5)   ///< Set by CPU on access.
#define PTE_DIRTY          (1UL << 6)   ///< Set by CPU on write.
#define PTE_HUGE           (1UL << 7)   ///< 2 MiB page (PD) / 1 GiB (PDPT).
#define PTE_GLOBAL         (1UL << 8)   ///< Don't flush from TLB on CR3 load.
#define PTE_NO_EXECUTE     (1UL << 63)  ///< NX: prevent instruction fetch.

/// Mask to extract the physical address from a PTE (bits 12-51).
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000UL

/**
 * @brief Common flag combinations
 *
 * The non-executable variants include @c PTE_NO_EXECUTE (NX) so that
 * data pages cannot be executed. The executable variants omit NX.
 */

#define PTE_KERNEL_RW   (PTE_PRESENT | PTE_WRITABLE | PTE_NO_EXECUTE)
#define PTE_KERNEL_RO   (PTE_PRESENT | PTE_NO_EXECUTE)
#define PTE_KERNEL_RWX  (PTE_PRESENT | PTE_WRITABLE)
#define PTE_KERNEL_ROX  (PTE_PRESENT)
#define PTE_MMIO        (PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE \
                         | PTE_NO_EXECUTE)
/// @}

/// @name Page table dimensions
/// @{
#define PT_ENTRIES  512
#define PT_SIZE     (PT_ENTRIES * sizeof(uint64_t))  // 4096 bytes
/// @}

/// @name Virtual address index extraction
/// @{

/// Extract the PML4 index (bits 47:39).
#define VMM_PML4_INDEX(addr)  (((uint64_t)(addr) >> 39) & 0x1FF)

/// Extract the PDPT index (bits 38:30).
#define VMM_PDPT_INDEX(addr)  (((uint64_t)(addr) >> 30) & 0x1FF)

/// Extract the PD index (bits 29:21).
#define VMM_PD_INDEX(addr)    (((uint64_t)(addr) >> 21) & 0x1FF)

/// Extract the PT index (bits 20:12).
#define VMM_PT_INDEX(addr)    (((uint64_t)(addr) >> 12) & 0x1FF)

/// Extract the page offset (bits 11:0).
#define VMM_PAGE_OFFSET(addr)  ((uint64_t)(addr) & 0xFFF)
/// @}

/// @name Page table entry type
/// @{
typedef uint64_t  pte_t;
typedef pte_t     page_table_t[PT_ENTRIES];
/// @}

/// @name Error codes
/// @{
#define VMM_OK              0
#define VMM_ERR_NO_MEMORY  (-1)  ///< PMM allocation failed for a page table.
#define VMM_ERR_ALREADY    (-2)  ///< Virtual address already mapped.
#define VMM_ERR_NOT_MAPPED (-3)  ///< Virtual address not mapped.
#define VMM_ERR_INVALID    (-4)  ///< Invalid argument (e.g., unaligned address).
/// @}

/// @name Initialization
/// @{

/**
 * @brief Initialize the virtual memory manager.
 *
 * Reads the current PML4 from CR3 and prepares the VMM for subsequent
 * map/unmap operations. Must be called after @c pmm_init().
 */
void vmm_init(void);
/// @}

/// @name Mapping / unmapping
/// @{

/**
 * @brief Map a single 4KB virtual page to a physical frame.
 *
 * @param vaddr  Virtual address (page-aligned).
 * @param paddr  Physical address (page-aligned).
 * @param flags  PTE flags: @c PTE_PRESENT is added automatically.
 * @return @c VMM_OK on success, or a @c VMM_ERR_* code.
 */
int vmm_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags);

/**
 * @brief Map @p count contiguous virtual pages to contiguous physical frames.
 *
 * @param vaddr  First virtual address (page-aligned).
 * @param paddr  First physical address (page-aligned).
 * @param flags  PTE flags.
 *
 * On failure, any pages mapped before the error are rolled back
 * (unmapped) before the error code is returned.
 *
 * @return @c VMM_OK on success, or a @c VMM_ERR_* code.
 */
int vmm_map_pages(uint64_t vaddr, uint64_t paddr, size_t count, uint64_t flags);

/**
 * @brief Unmap a single 4 KiB virtual page.
 *
 * Clears the PTE and flushes the TLB entry. Does @b not free the 
 * physical frame — the caller is responsible for calling 
 * @c pmm_free_page() if appropriate.
 *
 * @param vaddr  Virtual address to unmap (page-aligned).
 * @return Physical address that was mapped, or @c 0 if not mapped.
 */
uint64_t vmm_unmap_page(uint64_t vaddr);

/**
 * @brief Unmap @p count contiguous virtual pages.
 *
 * Unmaps each page and flushes its TLB entry. Does @b not free the
 * underlying physical frames.
 *
 * @param vaddr  First virtual address (page-aligned).
 * @param count  Number of pages to unmap.
 */
void vmm_unmap_pages(uint64_t vaddr, size_t count);
/// @}

/// @name Queries
/// @{

/**
 * @brief Look up the physical address for a virtual address.
 *
 * Handles 4 KiB, 2 MiB, and 1 GiB page sizes.
 *
 * @param vaddr  Virtual address to translate.
 * @return Physical address, or @c 0 if not mapped.
 */
uint64_t vmm_get_physical(uint64_t vaddr);

/**
 * @brief Check whether a virtual address is mapped.
 *
 * Handles 4 KiB, 2 MiB, and 1 GiB page sizes.
 *
 * @param vaddr  Virtual address to test.
 * @return @c true if a valid PTE exists for @p vaddr.
 */
bool vmm_is_mapped(uint64_t vaddr);

/**
 * @brief Update the PTE flags for an already-mapped page.
 *
 * Works on 4 KiB pages as well as 2 MiB / 1 GiB huge pages.
 * Flushes the TLB entry after updating.
 *
 * @param vaddr  Virtual address (page-aligned).
 * @param flags  New PTE flags; @c PTE_PRESENT (and @c PTE_HUGE for
 *               huge pages) are added automatically.
 * @return @c VMM_OK on success, or @c VMM_ERR_NOT_MAPPED.
 */
int vmm_set_flags(uint64_t vaddr, uint64_t flags);
/// @}

/// @name TLB Management
/// @{

/// Invalidate the TLB entry for a single virtual address.
static inline void vmm_flush_tlb(uint64_t vaddr)
{
    __asm__ volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

/**
 * @brief Flush the entire TLB by reloading CR3.
 *
 * @note Entries marked with @c PTE_GLOBAL survive a CR3 reload.
 *       To flush those, toggle CR4.PGE (not implemented here).
 */
static inline void vmm_flush_tlb_all(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/**
 * @brief Read the physical address of the current PML4 from CR3.
 * @return Physical address of the active PML4 (PCID / flags masked off).
 */
static inline uint64_t vmm_get_pml4(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3 & PTE_ADDR_MASK;
}
/// @}

#endif // EMMK_VMM_H

