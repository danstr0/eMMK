/**
 * @file tsc.c
 * @brief TSC calibration and time conversion.
 *
 * Calibration needs no interrupts: either CPUID leaf 0x15 states the
 * frequency outright, or the TSC is measured across a 50 ms PIT
 * channel 2 countdown, polled with interrupts disabled.
 */

#include "tsc.h"
#include "pit.h"
#include "spinlock.h"
#include "serial.h"

/// TSC frequency in Hz (set during calibration).
static uint64_t tsc_frequency;

/// Cycles per microsecond (rounded down).
static uint64_t cycles_per_us;

/// TSC value at the end of calibration.
static uint64_t tsc_boot;

/// Whether CPUID reports an invariant TSC.
static bool tsc_invariant;

uint64_t tsc_ns_mult;
uint64_t tsc_us_mult;
uint64_t tsc_ms_mult;

/// PIT measurement window (the PIT's 16-bit conter allows ≤ 54 ms).
#define CALIBRATION_MS  50

// ── CPUID helpers ──────────────────────────────────────────────────────

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                         uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

/**
 * @brief TSC frequency from CPUID leaf 0x15, if fully reported.
 *
 * Leaf 0x15 gives TSC/crystal ratio (EBX/EAX) and the crystal frequency
 * (ECX). Many CPUs leave ECX zero, in which case this returns @c 0.
 */
static uint64_t freq_from_cpuid(void)
{
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    if (a < 0x15)
        return 0;

    cpuid(0x15, &a, &b, &c, &d);
    if (a == 0 || b == 0 || c == 0)
        return 0;

    return (uint64_t)c * b / a;
}

/// TSC frequency measured against a PIT channel 2 one-shot.
static uint64_t freq_from_pit(void)
{
    uint64_t flags = irq_save();

    pit_oneshot_start(PIT_MS_TO_COUNT(CALIBRATION_MS));
    uint64_t t0 = tsc_read();
    while (!pit_oneshot_done())
        cpu_relax();
    uint64_t t1 = tsc_read();
    pit_oneshot_stop();

    irq_restore(flags);
    return (t1 - t0) * 1000 / CALIBRATION_MS;
}

// ── Initialization ─────────────────────────────────────────────────────

void tsc_init(void)
{
    uint32_t a, b, c, d;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000007)
    {
        cpuid(0x80000007, &a, &b, &c, &d);
        tsc_invariant = (d & (1U << 8)) != 0;
    }

    const char *source = "CPUID.15H";
    tsc_frequency = freq_from_cpuid();
    if (tsc_frequency == 0)
    {
        source = "PIT";
        tsc_frequency = freq_from_pit();
    }

    cycles_per_us = tsc_frequency / 1000000;
    if (cycles_per_us == 0)
        cycles_per_us = 1;

    // Fixed-point factors (see tsc.h)
    tsc_ns_mult = (1000000000UL << 32) / tsc_frequency;
    tsc_us_mult = (1000000UL << 40) / tsc_frequency;
    tsc_ms_mult = (1000UL << 48) / tsc_frequency;

    tsc_boot = tsc_read();

    kprintf("[TSC] %lu.%03lu MHz (via %s), %s\n",
            tsc_frequency / 1000000, (tsc_frequency / 1000) % 1000, source,
            tsc_invariant ? "invariant" : "invariance not reported");
}

bool tsc_is_invariant(void)
{
    return tsc_invariant;
}

uint64_t tsc_get_freq(void)
{
    return tsc_frequency;
}

uint64_t tsc_get_cycles_per_us(void)
{
    return cycles_per_us;
}

uint64_t tsc_get_boot(void)
{
    return tsc_boot;
}

void tsc_delay_us(uint64_t us)
{
    uint64_t target = tsc_read() + us * cycles_per_us;

    while (tsc_read() < target)
        cpu_relax();
}

