/**
 * @file test_interrupts.c
 * @brief Tests for the interrupt, APIC, and time subsystems.
 *
 * Runs after IDT, PIC, APIC, TSC, and timer init, with interrupts enabled
 * by the test runner. Interrupt delivery is exercised end-to-end with the
 * LAPIC timer and self-IPIs.
 */

#include "test.h"
#include "idt.h"
#include "apic.h"
#include "timer.h"
#include "tsc.h"

/// Vector for test self-IPIs (below APIC_WAKE_VECTOR in priority).
#define TEST_IPI_VECTOR  0xE0

static volatile bool int3_fired;
static volatile int  test_ipi_count;

static void test_int3_handler(struct interrupt_frame *frame)
{
    (void)frame;
    int3_fired = true;
}

static void test_ipi_handler(struct interrupt_frame *frame)
{
    (void)frame;
    test_ipi_count++;
    lapic_eoi();
}

/// @return @c true if RFLAGS.IF is set.
static bool interrupts_enabled(void)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
    return (rflags & (1UL << 9)) != 0;
}

/// Spin (with interrupts enabled) until @p cond or ~10 ms pass.
#define WAIT_FOR(cond)                                     \
    do                                                     \
    {                                                      \
        uint64_t _end = tsc_read() + tsc_get_freq() / 100; \
        while (!(cond) && tsc_read() < _end)               \
            cpu_relax();                                   \
    } while (0)

uint32_t test_interrupts(void)
{
    TEST_BEGIN("interrupts");

    // ── TSC clock ───────────────────────────────────────
    {
        uint64_t f = tsc_get_freq();
        TEST_ASSERT(f > 100000000UL, "TSC frequency calibrated (> 100 MHz)");

        // Fixed-point conversions agree with the frequency
        uint64_t ns = tsc_cycles_to_ns(f);
        uint64_t us = tsc_cycles_to_us(f);
        uint64_t ms = tsc_cycles_to_ms(f);
        TEST_ASSERT(ns > 999990000UL && ns <= 1000000000UL,
                    "1 s of cycles converts to ~1e9 ns");
        TEST_ASSERT(us > 999990UL && us <= 1000000UL,
                    "1 s of cycles converts to ~1e6 us");
        TEST_ASSERT(ms >= 999 && ms <= 1000,
                    "1 s of cycles converts to ~1000ms");

        uint64_t t0 = timer_get_ns();
        tsc_delay_us(2000);
        uint64_t t1 = timer_get_ns();
        TEST_ASSERT(t1 - t0 >= 2000000UL, "delay_us(2000) lasts >= 2 ms");
        TEST_ASSERT(t1 - t0 < 20000000UL, "delay_us(2000) returns promptly");
    }

    // ── Clock and sleeps don't need (or touch) interrupts
    {
        __asm__ volatile("cli");
        uint64_t ms0 = timer_get_ms();
        timer_sleep_ms(3);
        uint64_t ms1 = timer_get_ms();
        bool if_after = interrupts_enabled();
        __asm__ volatile("sti");

        TEST_ASSERT(ms1 - ms0 >= 3, "clock advances with interrupts off");
        TEST_ASSERT(!if_after, "timer_sleep_ms leaves IF unchanged");
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

    // ── LAPIC timer interrupts (cross-check vs TSC) ─────
    {
        lapic_timer_init(1000);
        uint64_t t0 = lapic_timer_get_ticks();
        timer_sleep_ms(50);
        uint64_t elapsed = lapic_timer_get_ticks() - t0;

        TEST_ASSERT(elapsed >= 35 && elapsed <= 70,
                    "LAPIC timer: ~50 interrupts in 50 ms");

        lapic_timer_stop();
        uint64_t stopped = lapic_timer_get_ticks();
        timer_sleep_ms(5);
        TEST_ASSERT(lapic_timer_get_ticks() - stopped <= 1,
                    "LAPIC timer stops");
    }

    // ── Self-IPIs and EOI ───────────────────────────────
    {
        idt_set_handler(TEST_IPI_VECTOR, test_ipi_handler);
        test_ipi_count = 0;

        lapic_send_ipi(lapic_id(), TEST_IPI_VECTOR);
        WAIT_FOR(test_ipi_count == 1);
        lapic_send_ipi(lapic_id(), TEST_IPI_VECTOR);
        WAIT_FOR(test_ipi_count == 2);
        TEST_ASSERT_EQ(test_ipi_count, 2, "self-IPI delivered twice");

        // A wake IPI must be acknowledged
        lapic_send_ipi(lapic_id(), APIC_WAKE_VECTOR);
        tsc_delay_us(1000);
        lapic_send_ipi(lapic_id(), TEST_IPI_VECTOR);
        WAIT_FOR(test_ipi_count == 3);
        TEST_ASSERT_EQ(test_ipi_count, 3,
                       "interrupts still delivered after a wake IPI");
    
        idt_set_handler(TEST_IPI_VECTOR, NULL);
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
        TEST_ASSERT(id < 256, "APIC ID < 256");
    }

    // ── Clocks is monotonic across multiple reads ───────
    {
        uint64_t prev = timer_get_ns();
        bool monotonic = true;
        for (int i = 0; i < 1000; ++i)
        {
            uint64_t cur = timer_get_ns();
            if (cur < prev)
            {
                monotonic = false;
                break;
            }
            prev = cur;
        }
        TEST_ASSERT(monotonic, "counter is monotonic");
    }

    TEST_END();
}

