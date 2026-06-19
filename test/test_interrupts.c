/**
 * @file test_interrupts.c
 * @brief Tests for the interrupt and timer subsystems.
 *
 * These tests run after IDT, PIC, APIC, and timer have been
 * initialized and interrupts are enabled (STI). The most important
 * test is the tick-counter check: if ticks are incrementing, the
 * entire interrupt pipeline works end-to-end.
 *
 * @pre Interrupts must be enabled before these tests run.
 */

#include "test.h"
#include "idt.h"
#include "apic.h"
#include "timer.h"

// ── INT3 handler test state ────────────────────────────────────────────

static volatile bool int3_fired;

static void test_int3_handler(struct interrupt_frame *frame)
{
    (void)frame;
    int3_fired = true;
}

uint32_t test_interrupts(void)
{
    TEST_BEGIN("interrupts");

    // ── Timer ticks are incrementing (end-to-end proof) ─
    {
        uint64_t t1 = timer_get_ticks();

        // Spin briefly to let at least one tick fire
        for (volatile int i = 0; i < 1000000; ++i)
            ;

        uint64_t t2 = timer_get_ticks();
        TEST_ASSERT(t2 > t1, "timer ticks incrementing");
    }

    // ── INT3 handler registration and dispatch ──────────
    {
        int3_fired = false;
        idt_set_handler(EXCEPTION_BP, test_int3_handler);

        __asm__ volatile("int3");

        TEST_ASSERT(int3_fired, "INT3 handler called");

        // Restore default (NULL = panic on unhandled exception)
        idt_set_handler(EXCEPTION_BP, NULL);
    }

    // ── timer_sleep_ms produces correct elapsed time ────
    {
        uint64_t before = timer_get_ticks();
        timer_sleep_ms(50);
        uint64_t after = timer_get_ticks();
        uint64_t elapsed = after - before;

        // Allow generous tolerance: 40-80 ticks for a 50ms sleep.
        // APIC timer calibration via PIT has ~5-10% imprecision,
        // and QEMU's virtual clock can jitter under host load.
        TEST_ASSERT(elapsed >= 40 && elapsed <= 80,
                    "sleep_ms(50) elapsed ~50 ticks");
    }

    // ── timer_delay_us doesn't hang ─────────────────────
    {
        uint64_t before = timer_get_ticks();
        timer_delay_us(500);
        uint64_t after = timer_get_ticks();

        // 500 us < 1 tick at 1000 Hz, so 0 or 1 ticks elapsed
        TEST_ASSERT(after >= before && after <= before + 2,
                    "delay_us(500) returned promptly");
    }

    // ── Deadline helpers ────────────────────────────────
    {
        uint64_t deadline = timer_deadline_ms(0);
        TEST_ASSERT(timer_deadline_expired(deadline),
                    "deadline_ms(0) immediately expired");

        deadline = timer_deadline_ms(5000);
        TEST_ASSERT(!timer_deadline_expired(deadline),
                    "deadline_ms(5000) not yet expired");
    }

    // ── lapic_id returns a plausible value ──────────────
    {
        uint32_t id = lapic_id();
        // BSP is typically APIC ID 0 on single-socket QEMU
        TEST_ASSERT(id < 256, "APIC ID < 256");
    }

    // ── Timer tick counter is monotonic across multiple reads
    {
        uint64_t prev = timer_get_ticks();
        bool monotonic = true;
        for (int i = 0; i < 100; ++i)
        {
            uint64_t cur = timer_get_ticks();
            if (cur < prev)
            {
                monotonic = false;
                break;
            }
            prev = cur;
        }
        TEST_ASSERT(monotonic, "tick counter is monotonic");
    }

    TEST_END();
}

