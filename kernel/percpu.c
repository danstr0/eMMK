/**
 * @file percpu.c
 * @brief Per-CPU data area management.
 *
 * Allocates a @c percpu struct for each core and sets the GS
 * segment base to point to it. After initialization, @c this_cpu()
 * returns the current core's percpu pointer via a single @c mov
 * from @c %gs:0.
 */

#include "percpu.h"
#include "tss.h"
#include "apic.h"
#include "heap.h"
#include "serial.h"
#include "string.h"

// ── MSR helper ─────────────────────────────────────────────────────────

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t low  = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

// ── Per-CPU storage ────────────────────────────────────────────────────

/**
 * @brief Static array of percpu pointers, indexed by core number.
 *
 * Used for cross-core access (e.g., sending an IPI to a specific
 * core and reading its state).
 */
static struct percpu *all_percpu[SMP_MAX_CORES];

/**
 * @brief Set the GS base for the current core.
 *
 * Writes the virtual address of the @c percpu struct into
 * @c MSR_GS_BASE. After this, @c %gs:0 reads @c percpu->self.
 */
static void set_gs_base(struct percpu *p)
{
    wrmsr(MSR_GS_BASE, (uint64_t)p);
}

/**
 * @brief Allocate and initialize a @c percpu struct.
 *
 * The struct is zeroed, then @c self is set to point to itself.
 */
static struct percpu *alloc_percpu(int core_index, uint32_t apic_id)
{
    struct percpu *p = kzalloc(sizeof(struct percpu));
    if (!p)
    {
        kprintf("[PERCPU] Failed to allocate percpu for core %d\n",
            core_index);
        return NULL;
    }

    p->self       = p;
    p->core_index = core_index;
    p->apic_id    = apic_id;
    p->tss        = tss_get(core_index);

    return p;
}

// ── Public API ─────────────────────────────────────────────────────────

void percpu_init_bsp(void)
{
    kprintf("[PERCPU] Initializing BSP per-CPU area...\n");

    uint32_t apic_id = lapic_id();
    struct percpu *p = alloc_percpu(0, apic_id);
    if (!p)
    {
        kprintf("[PERCPU] FATAL: Cannot allocate BSP percpu\n");
        return;
    }

    // Store the BSP's kernel stack top
    extern char stack_top[];
    p->rsp0 = (uint64_t)stack_top;

    all_percpu[0] = p;
    set_gs_base(p);

    kprintf("[PERCPU] BSP: core 0, APIC ID %lu, percpu at %p\n",
            (uint64_t)apic_id, (void *)p);
}

void percpu_init_ap(int core_index, uint32_t apic_id)
{
    if (core_index < 0 || core_index >= SMP_MAX_CORES)
        return;

    struct percpu *p = alloc_percpu(core_index, apic_id);
    if (!p)
        return;

    all_percpu[core_index] = p;
    set_gs_base(p);

    kprintf("[PERCPU] Core %d: APIC ID %lu, percpu at %p\n",
            core_index, (uint64_t)apic_id, (void *)p);
}

