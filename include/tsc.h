/**
 * @file tsc.h
 * @brief Time Stamp Counter — the kernel's clock.
 *
 * The TSC is a per-core 64-bit counter that increments at a fixed
 * rate. On modern x86-64 CPUs it is invariant (constant rate regardless
 * of power state) and synchronized across cores, which makes it both the
 * cheapest clock to read (one instruction, no memory access, no
 * interrupts) and the kernel's only time base: the timer API (timer.h)
 * is built on it.
 *
 * Provides:
 * - Raw TSC reads.
 * - One-time frequency calibration (CPUID leaf 0x15, else PIT channel 2).
 * - Division-free cycle → ns / µs / ms conversion via fixed-point
 *   multiply + shift.
 * - Microsecond busy-wait delays.
 *
 * @par Usage
 * @code{.c}
 *   uint64_t t0 = tsc_read();
 *   // ... hot path ...
 *   uint64_t t1 = tsc_read();
 *   uint64_t latency_ns = tsc_elapsed_ns(t0, t1);
 * @endcode
 *
 * TODO: cross-core comparisons assume the TSCs are synchronized.
 */

#ifndef EMMK_TSC_H
#define EMMK_TSC_H

#include "types.h"

/// @name Initialization
/// @{

/**
 * @brief Determine the TSC frequency and establish the boot time base.
 *
 * Uses CPUID leaf 0x15 when it reports a complete crystal ratio;
 * otherwise measures a 50 ms PIT channel 2 countdown.
 *
 * Must run before any timer.h function.
 */
void tsc_init(void);

/**
 * @return @c true if CPUID reports an invariant TSC (constant rate in all
 *         power states). Under QEMU's default CPU model this is false
 *         even though the emulated TSC behaves invariantly.
 */
bool tsc_is_invariant(void);
/// @}

/// @name Reading the TSC (hot path)
/// @{

/**
 * @brief Read the current TSC value.
 *
 * The leading @c lfence keeps the read from executing before earlier
 * instructions complete, so a measured region cannot leak out of the
 * front of a timing window.
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

/// @name Calibration results
/// @{

/// @return TSC frequency in Hz (0 before @c tsc_init()).
uint64_t tsc_get_freq(void);

/// @return TSC frequency in cycles per microsecond (rounded down).
uint64_t tsc_get_cycles_per_us(void);

/// @return TSC value captured at the end of @c tsc_init() (time zero).
uint64_t tsc_get_boot(void);
/// @}

/// @name Conversion (hot-path safe)
/// @{

// Fixed-point factors: units per cycle, scaled by 2^{shift}
extern uint64_t tsc_ns_mult;  ///< ns per cycle × 2^32
extern uint64_t tsc_us_mult;  ///< µs per cycle × 2^40
extern uint64_t tsc_ms_mult;  ///< ms per cycle × 2^48

/// Compute `(cycles × mult) >> shift` without overflow.
static inline uint64_t tsc_mul_shift(uint64_t cycles, uint64_t mult,
                                     unsigned shift)
{
    return (uint64_t)(((unsigned __int128)cycles * mult) >> shift);
}

/// Convert a cycle count to nanoseconds.
static inline uint64_t tsc_cycles_to_ns(uint64_t cycles)
{
    return tsc_mul_shift(cycles, tsc_ns_mult, 32);
}

/// Convert a cycle count to microseconds.
static inline uint64_t tsc_cycles_to_us(uint64_t cycles)
{
    return tsc_mul_shift(cycles, tsc_us_mult, 40);
}

/// Convert a cycle count to milliseconds.
static inline uint64_t tsc_cycles_to_ms(uint64_t cycles)
{
    return tsc_mul_shift(cycles, tsc_ms_mult, 48);
}

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

/// @name Delay
/// @{

/**
 * @brief Busy-wait for @p us microseconds.
 *
 * Works with interrupts disabled and never changes the interrupt flag.
 */
void tsc_delay_us(uint64_t us);
/// @}

#endif // EMMK_TSC_H

