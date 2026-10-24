/**
 * @file percpu.c
 * @brief Per-CPU data area management.
 */

#include "percpu.h"
#include "cpu.h"
#include "serial.h"

/// One area per possible core, each on its own cache line(s).
static struct percpu percpu_areas[SMP_MAX_CORES];

void percpu_init(int core_index, uint32_t apic_id)
{
    if (core_index < 0 || core_index >= SMP_MAX_CORES)
        return;

    struct percpu *p = &percpu_areas[core_index];
    p->self       = p;
    p->core_index = core_index;
    p->apic_id    = apic_id;
    p->tss        = tss_get(core_index);

    wrmsr(MSR_GS_BASE, (uint64_t)p);
}

void percpu_set_apic_id(uint32_t apic_id)
{
    this_cpu()->apic_id = apic_id;
}

struct percpu *percpu_get(int core_index)
{
    if (core_index < 0 || core_index >= SMP_MAX_CORES)
        return NULL;
    return &percpu_areas[core_index];
}

