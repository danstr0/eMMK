/**
 * @file kernel.h
 * @brief Shared kernel configuration, address layout, and page constants.
 *
 * Provides definitions used across multiple kernel subsystems (PMM, VMM,
 * drivers, etc.) that do not belong to any single module. Both @c pmm.h
 * and @c vmm.h include this header instead of depending on each other for
 * shared definitions.
 *
 * @par Centralized definitions
 * - Virtual address-space layout (kernel image, direct map, heap, MMIO)
 * - Physical ↔ virtual address conversion
 * - Page size / shift / mask / alignment helpers
 * - Page Frame Number (PFN) conversion
 *
 * @par Virtual address-space layout
 * @code{.txt}
 *   0x0000_0000_0000_1000 .. 0x0000_0000_001F_FFFF  Identity map
 *   0xFFFF_8000_0000_0000 .. +RAM                   Direct map of all RAM
 *   0xFFFF_C000_0000_0000 .. +HEAP_MAX_SIZE         Kernel heap
 *   0xFFFF_E000_0000_0000 .. +512 GiB               MMIO window (UC)
 *   0xFFFF_FFFF_8000_0000 .. +image                 Kernel image (-2 GiB)
 * @endcode
 * See docs/architecture/memory/memory_layout.rst for the full picture.
 */

#ifndef EMMK_KERNEL_H
#define EMMK_KERNEL_H

#include "types.h"

/// @name Virtual address-space layout
/// @{

/**
 * Kernel image base: PML4[511] + PDPT[510] = -2 GiB. The image is linked
 * at @c KERNEL_VIRTUAL_BASE + its physical load address (1 MiB).
 *
 * Must match the value used in the linker script (linker.ld) and in
 * the boot assembly (entry64.asm).
 */
#define KERNEL_VIRTUAL_BASE  0xFFFFFFFF80000000UL

/**
 * Direct map of physical memory (PML4[256]): physical address @c p is
 * accessible at @c DIRECT_MAP_BASE + @c p for every RAM region reported
 * by the bootloader. Established by @c vmm_init().
 */
#define DIRECT_MAP_BASE      0xFFFF800000000000UL

/// Size of the virtual window reserved for the direct map (64 TiB).
#define DIRECT_MAP_SIZE      (64UL << 40)

/// Kernel heap window (PML4[384]); see heap.h for its size limit.
#define HEAP_VIRTUAL_BASE    0xFFFFC00000000000UL

/// MMIO window (PML4[448]): device registers mapped by @c vmm_map_mmio().
#define MMIO_VIRTUAL_BASE    0xFFFFE00000000000UL

/// Size of the MMIO window (512 GiB, one PML4 entry).
#define MMIO_VIRTUAL_SIZE    (512UL << 30)
/// @}

/// @name Physical / virtual address conversion
///
/// - @c PHYS_TO_VIRT works for any RAM address, via the direct map. It is
///   valid only after @c vmm_init(); before that, use
///   @c BOOT_PHYS_TO_VIRT (boot tables, first 1 GiB only).
/// - @c VIRT_TO_PHYS works for direct-map addresses and kernel-image
///   addresses (statics, the boot stack). It is @b not valid for heap or
///   MMIO addresses — heap memory is not physically contiguous and must
///   never be handed to a device. Use @c virt_to_phys() (vmm.h) for an
///   arbitrary mapped address.
/// @{

/// Physical → virtual (direct map). Returns an integer.
#define PHYS_TO_VIRT_ADDR(paddr)  ((uint64_t)(paddr) + DIRECT_MAP_BASE)

/// Physical → virtual (direct map). Returns a pointer.
#define PHYS_TO_VIRT(paddr)  ((void *)PHYS_TO_VIRT_ADDR(paddr))

/**
 * @brief Typed pointer conversion from physical to virtual.
 *
 * @code{.c}
 *   struct page_table *pt = PHYS_TO_VIRT_PTR(paddr, struct page_table *);
 * @endcode
 */
#define PHYS_TO_VIRT_PTR(paddr, type)  ((type)(PHYS_TO_VIRT_ADDR(paddr)))

/**
 * @brief Virtual → physical for direct-map and kernel-image addresses.
 *
 * A compare and a subtraction; no page-table walk. Any address at or
 * above @c KERNEL_VIRTUAL_BASE is assumed to be in the kernel image, so
 * the result is meaningless for anything else; @c virt_to_phys() checks.
 */
static inline uint64_t virt_to_phys_fast(uint64_t vaddr)
{
    if (vaddr >= KERNEL_VIRTUAL_BASE)
        return vaddr - KERNEL_VIRTUAL_BASE;
    return vaddr - DIRECT_MAP_BASE;
}

#define VIRT_TO_PHYS_ADDR(vaddr)  virt_to_phys_fast((uint64_t)(vaddr))
#define VIRT_TO_PHYS(vaddr)       ((void *)(uintptr_t)VIRT_TO_PHYS_ADDR(vaddr))

/**
 * Early-boot physical → virtual, through the boot page tables'
 * higher-half window (first 1 GiB only). Used by the PMM and VMM before
 * the direct map exists.
 */
#define BOOT_PHYS_TO_VIRT(paddr)  ((void *)((uint64_t)(paddr) + KERNEL_VIRTUAL_BASE))

/// Highest physical address reachable through @c BOOT_PHYS_TO_VIRT.
#define BOOT_MAP_LIMIT            (1UL << 30)
/// @}

/// @name Page constants
/// @{
#define PAGE_SIZE   4096UL
#define PAGE_SHIFT  12
#define PAGE_MASK   (~(PAGE_SIZE - 1))

#define PAGE_SIZE_2M  (2UL << 20)  ///< Large page (PD-level mapping).
#define PAGE_SIZE_1G  (1UL << 30)  ///< Huge page (PDPT-level mapping).
/// @}

/// @name Page alignment
/// @{

/// Round @p addr up to the next page boundary.
#define PAGE_ALIGN_UP(addr)    ALIGN_UP((uint64_t)(addr), PAGE_SIZE)

/// Round @p addr down to the current page boundary.
#define PAGE_ALIGN_DOWN(addr)  ALIGN_DOWN((uint64_t)(addr), PAGE_SIZE)

/// True if @p addr is page-aligned.
#define PAGE_ALIGNED(addr)     (((uint64_t)(addr) & (PAGE_SIZE - 1)) == 0)
/// @}

/// @name Page Frame Number (PFN) conversion
/// @{

/// Convert a physical address to a page frame number.
#define ADDR_TO_PFN(addr)  ((uint64_t)(addr) >> PAGE_SHIFT)

/// Convert a page frame number to a physical address.
#define PFN_TO_ADDR(pfn)   ((uint64_t)(pfn) << PAGE_SHIFT)
/// @}

#endif // EMMK_KERNEL_H

