/**
 * @file tsc.c
 * @brief TSC calibration and time conversion.
 *
 * Calibrates the TSC by measuring cycles elapsed overa known number
 * of LAPIC timer ticks. The LAPIC timer fires at @c TIMER_HZ (1000 Hz),
 * so 100 ticks = 100ms — long enough for accurate calibration without
 * delaying boot noticeably.
 */

#include "tsc.h"
#include "timer.h"
#include "serial.h"

/// TSC frequency in Hz (set during calibration).
static uint64_t tsc_frequency;

/// Pre-computed cycles per microsecond (avoids division in conversions).
static uint64_t cycles_per_us;

/// Number of timer ticks to measure during calibration.
#define CALIBRATION_TICKS  100

void tsc_init(void)
{
    // Interrupts must be enabled
    __asm__ volatile("sti");

    // Wait for a tick boundary so the measurement starts cleanly
    uint64_t boundary = timer_get_ticks();
    while (timer_get_ticks() == boundary)
        __asm__ volatile("pause");

    uint64_t tsc_start  = tsc_read();
    uint64_t tick_start = timer_get_ticks();

    // Spin for CALIBRATION_TICKS timer interrupts (100 ms)
    while (timer_get_ticks() - tick_start < CALIBRATION_TICKS)
        __asm__ volatile("pause");

    uint64_t tsc_end      = tsc_read();
    uint64_t ticks_actual = timer_get_ticks() - tick_start;

    // frequency = cycles / seconds
    //           = cycles * ticks_per_second / ticks_elapsed
    //           = (tsc_end - tsc_start) * TIMER_HZ / ticks_actual
    uint64_t cycles = tsc_end - tsc_start;
    tsc_frequency   = cycles * TIMER_HZ / ticks_actual;
    cycles_per_us   = tsc_frequency / 1000000;

    // Safety: prevent division by zero in conversion functions
    if (cycles_per_us == 0)
        cycles_per_us = 1;

    kprintf("[TSC] Calibrated: %lu MHz (%lu cycles/us)\n",
            tsc_frequency / 1000000, cycles_per_us);
}

uint64_t tsc_get_freq(void)
{
    return tsc_frequency;
}

uint64_t tsc_get_cycles_per_us(void)
{
    return cycles_per_us;
}

uint64_t tsc_cycles_to_ns(uint64_t cycles)
{
    // ns = cycles * 1000 / cycles_per_us
    //
    // Multiplication before division preserves precision. Overflow
    // is not a concern for reasonable deltas: at 4 GHz, a 64-bit
    // counter overflows after ~146 years, and cycles * 1000 overflows
    // after ~146 million seconds (~4.6 years) of continuous measurement.
    return cycles * 1000 / cycles_per_us;
}

uint64_t tsc_cycles_to_us(uint64_t cycles)
{
    return cycles / cycles_per_us;
}

void tsc_delay_us(uint64_t us)
{
    uint64_t target = tsc_read() + us * tsc_get_cycles_per_us();

    while (tsc_read() < target)
        cpu_relax();
}

