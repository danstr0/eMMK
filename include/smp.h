/**
 * @file smp.h
 * @brief Symmetric Multi-Processing (SMP) support.
 *
 * Manages Application Processor (AP) startup and provides core
 * identification utilities. The Bootstrap Processor (BSP) calls
 * @c smp_init() after its own APIC and timer are configured; this
 * brings up all APs via the INIT-SIPI-SIPI sequence.
 *
 * @par AP startup sequence
 * 1. BSP copies the real-mode trampoline to physical address
 *    @c AP_TRAMPOLINE_ADDR.
 * 2. BSP writes boot parameters (PML4, GDT, entry point, stack)
 *    into the trampoline parameter block.
 * 3. BSP sends INIT IPI → waits 10 ms → SIPI → waits 200 µs → SIPI
 * 4. Each AP executes the trampoline: real mode → protected mode →
 *    long mode → C entry point @c ap_entry().
 * 5. AP loads segments, IDT, enables its local APIC, and enters
 *    its per-core run loop.
 *
 * @par Core numbering
 * Cores are numbered @c 0 .. @c smp_get_core_count()-1. Core 0 is
 * always the BSP. The mapping from APIC ID to core index is built
 * during @c smp_init().
 */

#ifndef EMMK_SMP_H
#define EMMK_SMP_H

#include "types.h"

/// @name Configuration
/// @{

/// Maximum number of CPU cores supported.
#define SMP_MAX_CORES  16

/// Physical address where the AP trampoline is copied.
/// Must be page-aligned, below 1 MiB, and not in use by the BIOS.
#define AP_TRAMPOLINE_ADDR  0x8000

/// Size of the per-AP boot stack (bytes).
#define AP_STACK_SIZE  (16 * 1024)
/// @}

/**
 * @brief Parameters written by the BSP into the trampoline page.
 *
 * The trampoline assembly reads these at fixed offsets relative to the
 * trampoline base address. the struct layout must match the offsets
 * used in @c ap_trampoline.asm.
 */
struct ap_boot_params
{
    uint32_t pml4_addr;     ///< Physical address of the PML4 (for CR3).
    uint32_t gdt_ptr_low;   ///< Linear address of the 64-bit GDT (low 32).
    uint32_t gdt_ptr_high;  ///< Linear address of the 64-bit GDT (high 32).
    uint16_t gdt_limit;     ///< GDT limit (size - 1).
    uint16_t pad0;
    uint64_t entry_point;   ///< 64-bit virtual address of @c ap_entry().
    uint64_t stack_top;     ///< Per-AP stack top (virtual address).
    uint32_t ap_ready;      ///< Set to @c 1 by the AP when it reaches C code.
    uint32_t ap_id;         ///< APIC ID assigned by the BSP before SIPI.
} PACKED;

/// Offset of the parameter block within the trampoline page.
/// Placed at the end of the 4 KiB page to avoid colliding with code.
#define AP_PARAMS_OFFSET  0xF00
/// @}

/// @name Core state
/// @{

/// Per-core information (indexed by core number, @b not APIC ID).
struct core_info
{
    uint32_t apic_id;     ///< Local APIC ID.
    bool     online;      ///< True once the core has reached @c ap_entry().
    bool     is_bsp;      ///< True for core 0.
    void    *stack_base;  ///< Base of the per-core stack allocation.
};
/// @}

/// @name Initialization
/// @{

/**
 * @brief Discover and start all Application Processors.
 *
 * Enumerates cores (assumes QEMU's APIC ID layout: 0, 1, 2, ...),
 * copies the trampoline, and brings up each AP via the INIT-SIPI-SIPI
 * sequence. Blocks until all APs have reported ready or a timeout expires.
 *
 * @pre The BSP's LAPIC, IDT, and timer must be initialized.
 * @pre Interrupts must be enabled (the timer is used for delays).
 *
 * @return Number of APs successfully started (0 if single-core).
 */
int smp_init(void);
/// @}

/// @name Core identification
/// @{

/// @return The total number of online cores (BSP + APs).
int smp_get_core_count(void);

/// @return The core index of the calling CPU (0 = BSP).
int smp_core_id(void);

/// @return The @c core_info for core @p index.
struct core_info *smp_get_core(int index);
/// @}

/// @name AP entry point
/// @{

/**
 * @brief C entry point for Application Processors.
 *
 * Called by the trampoline after the AP has entered 64-bit long mode
 * with its own stack. Performs per-core initialization (segments, IDT,
 * LAPIC) and enters the per-core run loop.
 *
 * This function never returns.
 */
NORETURN void ap_entry(void);
/// @}

#endif // EMMK_SMP_H

