/**
 * @file test_smp.c
 * @brief Tests for the SMP, per-CPU, and TSS subsystems.
 *
 * All tests execute on the BSP after @c smp_init() has completed. The
 * APs are in their idle loops and do not interfere. This verifies:
 * - per-CPU data: self-pointer, core_index, apic_id.
 * - TSS: allocation, IST stacks, GDT selector math.
 * - SMP state: core count, BSP identification, AP online flags.
 */

#include "test.h"
#include "percpu.h"
#include "tss.h"
#include "smp.h"
#include "apic.h"

uint32_t test_smp(void)
{
    TEST_BEGIN("smp");

    // ── Per-CPU data (BSP) ──────────────────────────────
    {
        struct percpu *p = this_cpu();
        TEST_ASSERT(p != NULL, "this_cpu() non-NULL");
        TEST_ASSERT_EQ((uintptr_t)p->self, (uintptr_t)p,
                       "percpu self-pointer");
        TEST_ASSERT_EQ(p->core_index, 0, "BSP percpu core_index == 0");
        TEST_ASSERT_EQ(p->apic_id, (uint32_t)lapic_id(),
                       "percpu apic_id matches LAPIC");
    }

    // ── this_core_id (GS-relative fast path) ────────────
    {
        TEST_ASSERT_EQ(this_core_id(), 0, "this_core_id == 0 on BSP");
        TEST_ASSERT_EQ(smp_core_id(), 0, "smp_core_id == 0 on BSP");
    }

    // ── TSS (BSP) ───────────────────────────────────────
    {
        struct tss *t = tss_get(0);
        TEST_ASSERT(t != NULL, "BSP TSS non-NULL");
        TEST_ASSERT(t->ist1 != 0, "BSP IST1 allocated");
        TEST_ASSERT(t->ist2 != 0, "BSP IST2 allocated");
        TEST_ASSERT(t->ist1 != t->ist2, "IST1 and IST2 are distinct");
        TEST_ASSERT_EQ(TSS_SELECTOR(0), 0x18, "BSP TSS selector 0x18");
        TEST_ASSERT_EQ(TSS_SELECTOR(1), 0x28, "core 1 TSS selector 0x28");

        // TSS percpu cross-reference
        struct percpu *p = this_cpu();
        TEST_ASSERT_EQ((uintptr_t)p->tss, (uintptr_t)t,
                       "percpu->tss matches tss_get(0)");
    }

    // ── SMP core state ──────────────────────────────────
    {
        int count = smp_get_core_count();
        TEST_ASSERT(count >= 1, "at least 1 core online");

        struct core_info *bsp = smp_get_core(0);
        TEST_ASSERT(bsp != NULL, "core 0 info non-NULL");
        TEST_ASSERT(bsp->is_bsp, "core 0 is BSP");
        TEST_ASSERT(bsp->online, "core 0 is online");

        // Invalid index returns NULL
        TEST_ASSERT_EQ((uintptr_t)smp_get_core(-1), (uintptr_t)NULL,
                       "smp_get_core(-1) == NULL");
        TEST_ASSERT_EQ((uintptr_t)smp_get_core(SMP_MAX_CORES),
                       (uintptr_t)NULL,
                       "smp_get_core(MAX) == NULL");
    }

    // ── AP verification ─────────────────────────────────
    {
        int count = smp_get_core_count();
        for (int i = 1; i < count; ++i)
        {
            struct core_info *ci = smp_get_core(i);
            TEST_ASSERT(ci != NULL, "AP core_info non-NULL");
            TEST_ASSERT(ci->online, "AP online");
            TEST_ASSERT(!ci->is_bsp, "AP is not BSP");
            TEST_ASSERT(ci->apic_id != 0 || i == 0,
                        "AP has non-zero APIC ID");

            struct tss *t = tss_get(i);
            TEST_ASSERT(t != NULL, "AP TSS non-NULL");
            TEST_ASSERT(t->ist1 != 0, "AP IST1 allocated");
            TEST_ASSERT(t->ist2 != 0, "AP IST2 allocated");
        }

        if (count > 1)
            kprintf("  verified %d AP(s)\n", count - 1);
        else
            kprintf("  single-core: AP tests skipped\n");
    }

    TEST_END();
}

