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
#include "irq.h"
#include "apic.h"
#include "cpu.h"
#include "timer.h"
#include "tsc.h"

/// Vector for test self-IPIs (below APIC_WAKE_VECTOR in priority).
#define TEST_IPI_VECTOR  0xE0

static volatile bool int3_fired;
static volatile int  test_ipi_count;
static void * volatile test_ipi_ctx;

static void test_int3_handler(struct interrupt_frame *frame)
{
    (void)frame;
    int3_fired = true;
}

static void test_ipi_handler(struct interrupt_frame *frame)
{
    test_ipi_ctx = irq_vector_ctx(frame->vector);
    test_ipi_count++;
    lapic_eoi();
}

/// Contexts for the vector-allocator tests.
static int ctx_a, ctx_b;

/// Scratch for the exhaustion test.
static int got_vectors[IRQ_DYN_COUNT];

static volatile uint64_t recover_rip;
static volatile int      pf_count;
static volatile uint64_t pf_addr;

/// A read-only object in .rodata to write to.
static const volatile uint8_t ro_target = 0x11;

/// #PF handler for tests: record the fault and resume at recover_rip.
static void test_pf_handler(struct interrupt_frame *frame)
{
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    pf_addr = cr2;
    pf_count++;
    frame->rip = recover_rip;
}

/// Store one byte at @p p; @return @c true if the store page-faulted.
static bool store_faults(volatile uint8_t *p)
{
    int before = pf_count;
    __asm__ volatile(
        "leaq 1f(%%rip), %%rax\n\t"
        "movq %%rax, %0\n\t"
        "movb $0x5a, (%1)\n\t"
        "1:\n\t"
        : "=m"(recover_rip)
        : "r"(p)
        : "rax", "memory");
    return pf_count != before;
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

    // ── Vector allocator ────────────────────────────────
    {
        int free0 = irq_vectors_free();
        TEST_ASSERT_EQ(free0, IRQ_DYN_COUNT,
                       "all dynamic vectors free before drivers");

        int a = irq_alloc_vector(test_ipi_handler, &ctx_a);
        int b = irq_alloc_vector(test_ipi_handler, &ctx_b);
        TEST_ASSERT(a >= IRQ_DYN_FIRST && a <= IRQ_DYN_LAST,
                    "vector in dynamic range");
        TEST_ASSERT(b >= IRQ_DYN_FIRST && b <= IRQ_DYN_LAST && b != a,
                    "second vector distinct and in range");
        TEST_ASSERT_EQ(irq_vectors_free(), free0 - 2,
                       "free count drops by 2");
        TEST_ASSERT(irq_vector_ctx(a) == &ctx_a
                 && irq_vector_ctx(b) == &ctx_b,
                   "per-vector context recorded");
        TEST_ASSERT(irq_vector_ctx(APIC_WAKE_VECTOR) == NULL,
                    "fixed vectors have no context");
        TEST_ASSERT_EQ(irq_alloc_vector(NULL, NULL), -1,
                       "NULL handler rejected");

        // Exhaust the range
        int n = 0;
        bool ok = true;
        int v;
        while (n < IRQ_DYN_COUNT
               && (v = irq_alloc_vector(test_ipi_handler, NULL)) >= 0)
        {
            if (v < IRQ_DYN_FIRST || v > IRQ_DYN_LAST || v == a || v == b
                || (n > 0 && v <= got_vectors[n - 1]))
                ok = false;
            got_vectors[n++] = v;
        }
        TEST_ASSERT_EQ(n, free0 - 2,
                       "allocates exactly the remaining vectors");
        TEST_ASSERT(ok, "vectors unique and within 64-239");
        TEST_ASSERT_EQ(irq_alloc_vector(test_ipi_handler, NULL), -1,
                       "exhausted range fails");
        for (int i = 0; i < n; ++i)
            irq_free_vector(got_vectors[i]);

        // Lowest-first allocation reuses a freed vector
        irq_free_vector(a);
        TEST_ASSERT(irq_vector_ctx(a) == NULL,
                    "freed vector loses its context");
        TEST_ASSERT_EQ(irq_alloc_vector(test_ipi_handler, &ctx_a), a,
                       "freed vector is reused");
        irq_free_vector(a);
        irq_free_vector(b);
        irq_free_vector(b);  // double free should be harmless
        irq_free_vector(IRQ_DYN_LAST + 1);  // out of range; ignored
        TEST_ASSERT_EQ(irq_vectors_free(), free0, "all vectors returned");
    }

    // ── Self-IPIs on an allocated vecttor, and EOI ──────
    {
        int vec = irq_alloc_vector(test_ipi_handler, &ctx_b);
        TEST_ASSERT(vec >= 0, "vector allocated for self-IPI test");
        test_ipi_count = 0;
        test_ipi_ctx = NULL;

        lapic_send_ipi(lapic_id(), (uint8_t)vec);
        WAIT_FOR(test_ipi_count == 1);
        TEST_ASSERT(test_ipi_ctx == &ctx_b,
                    "handler sees its vector's context");
        lapic_send_ipi(lapic_id(), (uint8_t)vec);
        WAIT_FOR(test_ipi_count == 2);
        TEST_ASSERT_EQ(test_ipi_count, 2, "self-IPI delivered twice");

        // service, every lower-priority vector would be blocked
        lapic_send_ipi(lapic_id(), APIC_WAKE_VECTOR);
        tsc_delay_us(1000);
        lapic_send_ipi(lapic_id(), (uint8_t)vec);
        WAIT_FOR(test_ipi_count == 3);
        TEST_ASSERT_EQ(test_ipi_count, 3,
                       "interrupts still delivered after a wake IPI");
    
        irq_free_vector(vec);
    }

    // ── Read-only pages are enforced in ring 0 (CR0.WP)
    {
        TEST_ASSERT(read_cr0() & CR0_WP, "CR0.WP set");

        idt_set_handler(EXCEPTION_PF, test_pf_handler);
        bool faulted = store_faults((volatile uint8_t *)&ro_target);
        idt_set_handler(EXCEPTION_PF, NULL);

        TEST_ASSERT(faulted, "store to .rodata page-faults");
        TEST_ASSERT_EQ(pf_addr, (uint64_t)&ro_target,
                       "fault address is the .rodata object");
        TEST_ASSERT_EQ(ro_target, 0x11, ".rodata value unchanged");
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

