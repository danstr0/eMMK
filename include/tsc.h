/**
 * @file tsc.h
 * @brief Time Stamp Counter — cycle-accurate timing.
 *
 * The TSC is a per-core 64-bit counter that increments at a fixed
 * rate (the CPU's reference frequency). On modern x86-64 CPUs the
 * TSC is invariant (constant rate regardless of power state) and
 * synchronized across cores, making it ideal for low-overhead
 * latency measurement.
 *
 * Provides:
 * - Raw TSC reads.
 * - Frequency calibration.
 * - Cycle/time conversions.
 * - Microsecond busy-wait delays.
 * - High-resolution latency measurement.
 *
 * @par Usage
 * @code
 *   uint64_t t0 = tsc_read();
 *   // ... hot path ...
 *   uint64_t t1 = tsc_read();
 *   uint64_t latency_ns = tsc_elapsed_ns(t0, t1);
 * @endcode
 *
 * @par Hot-path guidelines
 * Record raw TSC values on the hot path (single instruction, no
 * memory access). Convert to nanoseconds only during reporting —
 * the conversion uses division and is not suited for per-packet
 * inline use.
 */

#ifndef EMMK_TSC_H
#define EMMK_TSC_H

#include "types.h"

/// @name Initialization
/// @{

/**
 * @brief Calibrate the TSC against the LAPIC timer.
 *
 * Measures TSC ticks over a known time interval to determine the
 * TSC frequency. Must be called after @c timer_init() with
 * interrupts enabled.
 */
void tsc_init(void);
/// @}

/// @name Reading the TSC (hot path)
/// @{

/**
 * @brief Read the current TSC value.
 *
 * @return 64-bit cycle count.
 */
static inline uint64_t tsc_read(void)
{
    uint32_t lo, hi;
    __asm__ volatile(
            "lfence\n\t"
            "rdtsc"
            : "=a"(lo), "=d"(hi)
            :
            : "memory");
    return ((uint64_t)hi << 32) | lo;
}
/// @}

/// @name Conversion (reporting path — not for hot-path use)
/// @{

/// @return TSC frequency in Hz.
uint64_t tsc_get_freq(void);

/// @return TSC frequency in cycles per microsecond.
uint64_t tsc_get_cycles_per_us(void);

/// Convert a cycle count to nanoseconds.
uint64_t tsc_cycles_to_ns(uint64_t cycles);

/// Convert a cycle count to microseconds.
uint64_t tsc_cycles_to_us(uint64_t cycles);

/// Compute elapsed nanoseconds between two TSC snapshots.
static inline uint64_t tsc_elapsed_ns(uint64_t start, uint64_t end)
{
    return tsc_cycles_to_ns(end - start);
}

/// Compute elapsed microseconds between two TSC snapshots.
static inline uint64_t tsc_elapsed_us(uint64_t start, uint64_t end)
{
    return tsc_cycles_to_us(end - start);
}
/// @}

/// @name Deadline helper
/// @{

/**
 * @brief Busy-wait for @p us microseconds
 *
 * Works even with interrupts disabled.
 */
void tsc_delay_us(uint64_t us);
/// @}

#endif // EMMK_TSC_H

