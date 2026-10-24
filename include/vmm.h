/**
 * @file vmm.h
 * @brief Virtual Memory Manager (VMM) interface.
 *
 * Builds and manages the kernel's 4-level (PML4) page tables on x86-64.
 * Provides functions to map, unmap, and query virtual-to-physical
 * translations, a direct map of all RAM, and an uncached MMIO mapper.
 *
 * @par Page table hierarchy (4 KiB pages)
 * @code{.txt}
 *   PML4 -> PDPT ->  PD  -> PT   -> Physical Page
 *   [47:39] [38:30] [29:21] [20:12] [11:0] offset
 *               │       └─ PS=1: 2 MiB page
 *               └───────── PS=1: 1 GiB page
 * @endcode
 *
 * @par Address space
 * See kernel.h for the virtual layout. @c vmm_init() replaces the boot
 * page tables with the kernel's own: per-section permissions for the
 * kernel image, a guard page below the boot stack, an unmapped NULL page,
 * and a direct map of all RAM using the largest pages that fit.
 *
 * @par Concurrency
 * All modifications are serialized by an internal spinlock. Queries do
 * not take the lock.
 *
 * @warning There is no TLB shootdown. Changing or removing a mapping
 *          that other cores may have cached (after @c smp_init()) only
 *          invalidates the calling core's TLB. Creating a new mapping is
 *          safe: x86 does not cache not-present translations.
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
#define PTE_WRITE_THROUGH  (1UL << 3)   ///< PWT: write-through caching.
#define PTE_CACHE_DISABLE  (1UL << 4)   ///< PCD: disable caching.
#define PTE_ACCESSED       (1UL << 5)   ///< Set by CPU on access.
#define PTE_DIRTY          (1UL << 6)   ///< Set by CPU on write.
#define PTE_HUGE           (1UL << 7)   ///< PS: 2 MiB page (PD) / 1 GiB (PDPT).
#define PTE_GLOBAL         (1UL << 8)   ///< Don't flush from TLB on CR3 load.
#define PTE_NO_EXECUTE     (1UL << 63)  ///< NX: prevent instruction fetch.

/// Mask to extract the physical address from a PTE (bits 12-51).
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000UL

/**
 * @brief Common flag combinations
 *
 * The non-executable variants include @c PTE_NO_EXECUTE (NX) so that
 * data pages cannot be executed. The executable variants omit NX.
 *
 * @c PTE_MMIO sets both PCD and PWT, which selects PAT entry 3 (strong
 * UC with the power-on PAT): every access goes to the device, in order,
 * and is never combined or speculatively read.
 */
#define PTE_KERNEL_RW   (PTE_PRESENT | PTE_WRITABLE | PTE_NO_EXECUTE)
#define PTE_KERNEL_RO   (PTE_PRESENT | PTE_NO_EXECUTE)
#define PTE_KERNEL_RWX  (PTE_PRESENT | PTE_WRITABLE)
#define PTE_KERNEL_ROX  (PTE_PRESENT)
#define PTE_MMIO        (PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE \
                         | PTE_WRITE_THROUGH | PTE_NO_EXECUTE)
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
#define VMM_ERR_ALREADY    (-2)  ///< A conflicting mapping already exists.
#define VMM_ERR_NOT_MAPPED (-3)  ///< Virtual address not mapped.
#define VMM_ERR_INVALID    (-4)  ///< Invalid argument, or a large page is
                                 ///< in the way of a 4 KiB operation.
/// @}

/// @name Initialization
/// @{

/**
 * @brief Build the kernel page tables and switch to them.
 *
 * Must be called after @c pmm_init() and before anything that uses
 * @c PHYS_TO_VIRT(). Creates, in a fresh PML4:
 * - the kernel image at @c KERNEL_VIRTUAL_BASE with per-section
 *   permissions (text RX, rodata RO, data RW+NX); the boot page tables
 *   are left unmapped, forming a guard page below the boot stack;
 * - an identity map of [4 KiB, 2 MiB) for the AP trampoline (page 0 is
 *   unmapped so NULL dereferences fault);
 * - the direct map of every RAM region at @c DIRECT_MAP_BASE.
 *
 * Then loads CR3 and switches the PMM to the direct map. The boot page
 * tables (and the boot higher-half window beyond the kernel image) are
 * no longer reachable afterwards.
 */
void vmm_init(void);

/// @return @c true if the CPU supports 1 GiB pages (CPUID pdpe1gb).
bool vmm_has_1g_pages(void);
/// @}

/// @name Mapping / unmapping
/// @{

/**
 * @brief Map a single 4 KiB virtual page to a physical frame.
 *
 * @param vaddr  Virtual address (page-aligned).
 * @param paddr  Physical address (page-aligned).
 * @param flags  PTE flags: @c PTE_PRESENT is added automatically.
 * @return @c VMM_OK on success, @c VMM_ERR_NO_MEMORY if a page table
 *         could not be allocated, or @c VMM_ERR_INVALID if a 2 MiB /
 *         1 GiB page already covers @p vaddr.
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
 * @brief Map a physically contiguous range with the largest page sizes
 *        that fit.
 *
 * Uses 1 GiB pages (if supported) and 2 MiB pages wherever @p vaddr and
 * @p paddr are both suitably aligned and enough of the range remains,
 * and 4 KiB pages elsewhere. Fewer, larger pages mean fewer TLB misses.
 *
 * @param vaddr  Virtual start (page-aligned).
 * @param paddr  Physical start (page-aligned).
 * @param size   Length in bytes (multiple of @c PAGE_SIZE).
 * @param flags  PTE flags (@c PTE_HUGE is managed internally).
 * @return @c VMM_OK, or a @c VMM_ERR_* code (no rollback).
 */
int vmm_map_range(uint64_t vaddr, uint64_t paddr, size_t size, uint64_t flags);

/**
 * @brief Allocate a kernel stack with a guard page below it.
 *
 * Maps @p size bytes (rounded up to pages) of fresh RW+NX memory in the
 * stack window, leaving the page just below unmapped: overflowing the
 * stack faults (#PF, which escalates to #DF on its IST stack) instead of
 * silently overwriting whatever lies below. Frames are not physically
 * contiguous. Stacks are never freed.
 *
 * @return The stack top (initial RSP), or @c 0 on failure.
 */
uint64_t vmm_alloc_stack(size_t size);

/**
 * @brief Remove the [4 KiB, 2 MiB) identity map.
 *
 * Called by @c smp_init() once every AP has left the trampoline.
 * APs flush their own TLBs before running their roles.
 */
void vmm_unmap_identity(void);

/**
 * @brief Map a device register region uncached into the MMIO window.
 *
 * Allocates fresh virtual space for every call (regions are never
 * shared or reused), maps it with @c PTE_MMIO, and leaves and unmapped
 * guard page after it.
 *
 * @param paddr  Physical address of the region (need not be aligned).
 * @param size   Length in bytes.
 * @return Pointer corresponding to @p paddr, or @c NULL on failure.
 */
void *vmm_map_mmio(uint64_t paddr, size_t size);

/**
 * @brief Unmap the page that starts at @p vaddr.
 *
 * Normally a 4 KiB page; if @p vaddr is the first byte of a 2 MiB or
 * 1 GiB page, that whole page is unmapped. An address inside a large
 * page (not its start) is rejected.
 *
 * Clears the entry and flushes the local TLB entry. Does @b not free
 * the physical frame(s) — the caller is responsible for that.
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
 * @brief Translate any mapped kernel pointer to a physical address.
 *
 * Direct-map and kernel-image addresses are converted arithmetically;
 * anything else (heap, MMIO) takes a page-table walk.
 *
 * @return Physical address, or @c 0 if not mapped.
 */
uint64_t virt_to_phys(const void *ptr);

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
 * @brief Return the flags of the leaf entry mapping @p vaddr.
 *
 * @return The entry with its address bits masked off (includes
 *         @c PTE_HUGE for 2 MiB / 1 GiB pages), or 0 if not mapped.
 */
uint64_t vmm_get_flags(uint64_t vaddr);

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

