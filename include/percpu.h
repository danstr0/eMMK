/**
 * @file percpu.h
 * @brief Per-CPU data area.
 *
 * Each CPU core has a private @c percpu instance, accessible at zero
 * cost via the GS segment base register. On x86-64, the @c MSR_GS_BASE
 * MSR is set during core init to point to the core's percpu struct;
 * reading @c %gs:offset then gives direct access without any table
 * lookup.
 *
 * @par Accessing per-CPU data
 * @code
 *   struct percpu *p = this_cpu();
 *   int id = p->core_index;
 * @endcode
 *
 * @par Why GS and not FS?
 * The Linux kernel uses GS for per-CPU data in kernel mode. Since eMMK
 * has no userland (and no SWAPGS), GS base is set directly via @c wrmsr
 * and FS is left unused.
 *
 * @note The @c percpu struct is allocated from the heap during core init.
 *       The BSP's percpu is set up during @c percpu_init_bsp(); each
 *       AP's is set up during @c percpu_init_ap().
 */

#ifndef EMMK_PERCPU_H
#define EMMK_PERCPU_H

#include "types.h"
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
 * This allows a single instruction to load the @c percpu pointer from
 * any context.
 */
struct percpu
{
    struct percpu *self;        ///< Points to this struct (for @c this_cpu()).
    int            core_index;  ///< Core number (0 = BSP).
    uint32_t       apic_id;     ///< Local APIC ID.
    struct tss    *tss;         ///< This core's TSS.
    uint64_t       rsp0;        ///< Kernel stack top (for RSP0 in TSS).

    // Future per-core fields go here:
    //   - per-core PMM bitmap / free list
    //   - per-core slab caches
    //   - per-core netdev RX queue
    //   - per-core statistics / counters
};
/// @}

/// @name Accessor
/// @{

/**
 * @brief Get the current core's @c percpu pointer.
 *
 * Reads @c %gs:0, which contains @c percpu->self (a pointer back to
 * the struct). This compiles to a single @c mov instruction.
 */
static inline struct percpu *this_cpu(void)
{
    struct percpu *p;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(p));
    return p;
}

/**
 * @brief Get the current core's index (0 = BSP).
 *
 * Slightly cheaper than @c this_cpu()->core_index because it reads
 * directly from the GS-relative offset without materializing the
 * full pointer.
 */
static inline int this_core_id(void)
{
    int id;
    __asm__ volatile("movl %%gs:%c1, %0"
                     : "=r"(id)
                     : "i"(__builtin_offsetof(struct percpu, core_index)));
    return id;
}
/// @}

/// @name Initialization
/// @{

/**
 * @brief Allocate and install the BSP's @c percpu area.
 *
 * Must be called after @c heap_init() and before @c smp_init().
 * Sets up the GS base for core 0.
 */
void percpu_init_bsp(void);

/**
 * @brief Allocate and install an AP's percpu area.
 *
 * Called from @c ap_entry() on each Application Processor.
 *
 * @param core_index  The AP's core index.
 * @param apic_id     The AP's Local APIC ID.
 */
void percpu_init_ap(int core_index, uint32_t apic_id);
/// @}

#endif // EMMK_PERCPU_H

