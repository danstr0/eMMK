/**
 * @file tss.h
 * @brief Task State Segment (TSS) management.
 *
 * In 64-bit long mode the TSS serves two purposes:
 * - **RSP0-RSP2:** stack pointers loaded on privilege-level changes
 *   (not used by eMMK — ring 0 only, but RSP0 must still be valid).
 * - **IST1-IST7:** Interrupt Stack Table entries that allow specific
 *   IDT gates to switch to a known-good stack on entry (critical for
 *   double-fault and NMI handling).
 *
 * Each CPU core requires its own TSS because the hardware reads it on
 * every interrupt. The GDT must contain one 16-byte TSS descriptor
 * per core, and each core loads its own via @c LTR.
 *
 * @par GDT layout (after TSS init)
 * @code
 *   Slot 0:     Null descriptor       (8 bytes)
 *   Slot 1:     64-bit code segment   (8 bytes)    selector 0x08
 *   Slot 2:     64-bit data segment   (8 bytes)    selector 0x10
 *   Slot 3-4:   TSS for core 0        (16 bytes)   selector 0x18
 *   Slot 5-6:   TSS for core 1        (16 bytes)   selector 0x28
 *   ...
 * @endcode
 *
 * @see Intel SDM Vol 3, §8.2 (Task Management Data Structures),
 *      §6.14.5 (Interrupt Stack Table).
 */

#ifndef EMMK_TSS_H
#define EMMK_TSS_H

#include "types.h"
#include "kernel.h"

/// @name TSS structure
/// @{

/**
 * @brief 64-bit Task State Segment (104 bytes).
 *
 * Only the RSP and IST fields are used by the CPU in long mode.
 * All other fields are reserved and must be zero.
 */
struct tss
{
    uint32_t reserved0;
    uint64_t rsp0;         ///< Stack pointer for ring 0.
    uint64_t rsp1;         ///< Stack pointer for ring 1 (unused).
    uint64_t rsp2;         ///< Stack pointer for ring 2 (unused).
    uint64_t reserved1;
    uint64_t ist1;         ///< IST entry 1 (double fault).
    uint64_t ist2;         ///< IST entry 2 (NMI).
    uint64_t ist3;         ///< IST entry 3 (available).
    uint64_t ist4;         ///< IST entry 4 (available).
    uint64_t ist5;         ///< IST entry 5 (available).
    uint64_t ist6;         ///< IST entry 6 (available).
    uint64_t ist7;         ///< IST entry 7 (available).
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;  ///< I/O permission bitmap offset (unused).
} PACKED;

STATIC_ASSERT(sizeof(struct tss) == 104, "TSS must be 104 bytes");
/// @}

/// @name IST indices
/// @{

/// IST entry used for double-fault (\#DF) handling.
#define IST_DOUBLE_FAULT  1

/// IST entry used for NMI handling.
#define IST_NMI           2

/// Size of each IST stack (bytes). Allocated with a guard page below.
#define IST_STACK_SIZE    (8 * 1024)
/// @}

/// @name GDT layout
/// @{

/// Number of base GDT entries before the TSS descriptors.
#define GDT_BASE_ENTRIES  3

/// Each TSS descriptor occupies two GDT slots (16 bytes) in long mode.
#define GDT_TSS_SLOTS     2

/// Maximum GDT entries: 3 base + 2 per core.
#define GDT_MAX_ENTRIES   (GDT_BASE_ENTRIES + SMP_MAX_CORES * GDT_TSS_SLOTS)

/// TSS descriptor selector for a given core index.
#define TSS_SELECTOR(core) \
    ((uint16_t)((GDT_BASE_ENTRIES + (core) * GDT_TSS_SLOTS) * 8))
/// @}

/// @name Initialization
/// @{

/**
 * @brief Build the runtime GDT with TSS descriptors and load it.
 *
 * Replaces the boot GDT (from multiboot2.asm) with a larger GDT that
 * has room for per-core TSS descriptors. Copies the original null,
 * code, and data descriptors, then installs a TSS descriptor for the
 * BSP and loads TR.
 *
 * Must be called on the BSP before @c smp_init(). Each AP calls
 * @c tss_init_ap() to install and load its own TSS.
 */
void tss_init(void);

/**
 * @brief Install and load the TSS for an Application Processor.
 *
 * Called from @c ap_entry() on each AP after segments and the IDT
 * are loaded. Allocates IST stacks, fills the TSS, writes the GDT
 * descriptor, and loads TR.
 *
 * @param core_index  The core's index (@c 1 .. @c core_count-1).
 * @param stack_top   Top of the AP's kernel stack (becomes RSP0).
 */
void tss_init_ap(int core_index, uint64_t stack_top);
/// @}

/// @name Per-core TSS access
/// @{

/**
 * @brief Get the TSS for a given core.
 * @param core_index  Core index (0 = BSP).
 * @return Pointer to the core's TSS, or @c NULL if invalid.
 */
struct tss *tss_get(int core_index);
/// @}

#endif // EMMK_TSS_H

