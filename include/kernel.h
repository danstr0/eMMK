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
 * - Kernel virtual base address
 * - Physical ↔ virtual address conversion macros
 * - Page size / shift / mask / alignment helpers
 * - Page Frame Number (PFN) conversion
 */

#ifndef EMMK_KERNEL_H
#define EMMK_KERNEL_H

#include "types.h"

/// @name Kernel address layout
/// @{

/**
 * Higher-half virtual base address: PML4[511] + PDPT[510] = -2 GiB
 *
 * Must match the value used in the linker script (linker.ld) and in
 * the boot assembly (multiboot2.asm / entry64.asm).
 */
#define KERNEL_VIRTUAL_BASE  0xFFFFFFFF80000000UL
/// @}

/// @name Physical / virtual address conversion
/// @{

/**
 * Integer conversions (for arithmetic on raw addresses).
 * These return @c uint64_t values.
 */

#define PHYS_TO_VIRT_ADDR(paddr)  ((uint64_t)(paddr) + KERNEL_VIRTUAL_BASE)
#define VIRT_TO_PHYS_ADDR(vaddr)  ((uint64_t)(vaddr) - KERNEL_VIRTUAL_BASE)

/**
 * Pointer conversions (for dereferencing mapped memory).
 * These return @c void* values.
 */

#define PHYS_TO_VIRT(paddr)  ((void *)PHYS_TO_VIRT_ADDR(paddr))
#define VIRT_TO_PHYS(vaddr)  ((void *)(uintptr_t)VIRT_TO_PHYS_ADDR(vaddr))

/**
 * @brief Typed pointer conversion from physical to virtual.
 *
 * @code
 *   struct page_table *pt = PHYS_TO_VIRT_PTR(paddr, struct page_table *);
 * @endcode
 */
#define PHYS_TO_VIRT_PTR(paddr, type)  ((type)(PHYS_TO_VIRT_ADDR(paddr)))
/// @}

/// @name Page constants
/// @{
#define PAGE_SIZE   4096UL
#define PAGE_SHIFT  12
#define PAGE_MASK   (~(PAGE_SIZE - 1))
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

