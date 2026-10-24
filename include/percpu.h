/**
 * @file percpu.h
 * @brief Per-CPU data area.
 *
 * Each CPU core has a private @c percpu instance, accessible at zero
 * cost via the GS segment base register: @c MSR_GS_BASE points at the
 * core's struct, so @c %gs:offset reads a field directly with no table
 * lookup.
 *
 * @par Accessing per-CPU data
 * @code{.c}
 *   struct percpu *p = this_cpu();
 *   int id = this_core_id();        // one instruction: movl %gs:8, %eax
 * @endcode
 *
 * @par Lifetime
 * The areas are a static, cache-line-aligned array (one per possible
 * core), so they need no allocator and exist from the first instruction.
 * Each core installs its own as the very first thing it does: the BSP at
 * the top of @c kernel_main(), each AP at the top of @c ap_entry(). From
 * then on @c this_core_id() is valid everywhere, including in the slab
 * allocator's fast path an in exception handlers.
 *
 * @par Why GS and not FS?
 * The Linux kernel uses GS for per-CPU data in kernel mode. Since eMMK
 * has no userland (and no SWAPGS), GS base is set directly via @c wrmsr
 * and FS is left unused.
 */

#ifndef EMMK_PERCPU_H
#define EMMK_PERCPU_H

#include "types.h"
#include "kernel.h"
#include "tss.h"

/// MSR for the GS segment base address.
#define MSR_GS_BASE  0xC0000101

/// @name Per-CPU structure
/// @{

/**
 * @brief Per-core state, accessed via GS base.
 *
 * The @c self pointer must be the first field — @c this_cpu() reads
 * @c %gs:0 and expects to get a pointer back to the struct itself.
 *
 * Cache-line aligned so that no two cores' areas share a line: fields a
 * core writes for itself never invalidate another core's cache.
 */
struct percpu
{
    struct percpu *self;        ///< Points to this struct (for @c this_cpu()).
    int            core_index;  ///< Core number (0 = BSP).
    uint32_t       apic_id;     ///< Local APIC ID.
    struct tss    *tss;         ///< This core's TSS.
    // Future per-core fields
} CACHE_ALIGNED;
/// @}

/// @name Accessors
/// @{

/**
 * @brief Get the current core's @c percpu pointer.
 *
 * Reads @c %gs:0, which contains @c percpu->self.
 */
static inline struct percpu *this_cpu(void)
{
    struct percpu *p;
    __asm__("movq %%gs:0, %0" : "=r"(p));
    return p;
}

/**
 * @brief Get the current core's index (0 = BSP).
 *
 * Reads the field directly from its GS-relative offset.
 */
static inline int this_core_id(void)
{
    int id;
    __asm__ ("movl %%gs:%c1, %0"
             : "=r"(id)
             : "i"(__builtin_offsetof(struct percpu, core_index)));
    return id;
}
/// @}

/// @name Initialization and cross-core access
/// @{

/**
 * @brief Install the per-CPU area for the calling core.
 *
 * Fills in @c percpu[core_index] and points @c MSR_GS_BASE at it. Needs
 * no allocator, LAPIC, or other subsystem.
 *
 * @param core_index  Core number (0 = BSP).
 * @param apic_id     Local APIC ID.
 */
void percpu_init(int core_index, uint32_t apic_id);

/// Update the calling core's recorded APIC ID.
void percpu_set_apic_id(uint32_t apic_id);

/**
 * @brief Another core's per-CPU area.
 * @return Pointer to core @p core_index's area, or @c NULL if out of range.
 */
struct percpu *percpu_get(int core_index);
/// @}

#endif // EMMK_PERCPU_H

