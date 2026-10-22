/**
 * @file timer.h
 * @brief Kernel time API.
 *
 * All time is derived from the TSC (see tsc.h). There is no periodic
 * timer interrupt: the kernel is tickless, so no core is ever interrupted
 * to keep time, and reading the clock works with interrupts disabled.
 *
 * Provides:
 * - Monotonic time since boot in ns / µs / ms.
 * - A millisecond "tick" counter (@c TIMER_HZ = 1000) for code that
 *   stores coarse timestamps (e.g., ARP cache ages).
 * - Busy-wait sleeps.
 * - Deadline-based timeout helpers.
 *
 * @warning On a run-to-completion kernel, all sleep / delay functions
 *          are busy-wait (polling) loops. Calling them blocks all
 *          processing on the current core for the duration. Use
 *          sparingly during initialization — prefer deadline-based
 *          timeout checks in packet-processing paths.
 */

#ifndef EMMK_TIMER_H
#define EMMK_TIMER_H

#include "types.h"
#include "tsc.h"

/// @name Configuration
/// @{

/**
 * @brief Resolution of the tick counter: 1000 ticks per second.
 *
 * Ticks are derived from the TSC, not counted by an interrupt.
 */
#define TIMER_HZ  1000
/// @}

/// @name Initialization
/// @{

/**
 * @brief Initialize the time subsystem.
 *
 * @pre @c tsc_init() has run (it establishes time zero and the
 *      conversion factors).
 */
void timer_init(void);
/// @}

/// @name Monotonic clock
/// @{

/// Nanoseconds since @c tsc_init().
static inline uint64_t timer_get_ns(void)
{
    return tsc_cycles_to_ns(tsc_read() - tsc_get_boot());
}

/// Microseconds since @c tsc_init().
static inline uint64_t timer_get_us(void)
{
    return tsc_cycles_to_us(tsc_read() - tsc_get_boot());
}

/// Milliseconds since @c tsc_init().
static inline uint64_t timer_get_ms(void)
{
    return tsc_cycles_to_ms(tsc_read() - tsc_get_boot());
}

/**
 * @brief Read the monotonic tick counter.
 *
 * Kept for coarse timestamps; equal to @c timer_get_ms().
 */
static inline uint64_t timer_get_ticks(void)
{
    return timer_get_ms();
}

/// Seconds since boot (truncated).
static inline uint64_t timer_get_uptime(void)
{
    return timer_get_ms() / 1000;
}

/// Convert a tick count to milliseconds.
static inline uint64_t timer_ticks_to_ms(uint64_t ticks)
{
    return ticks * 1000 / TIMER_HZ;
}

/// Convert milliseconds to a tick count.
static inline uint64_t timer_ms_to_ticks(uint64_t ms)
{
    return ms * TIMER_HZ / 1000;
}
/// @}

/// @name Busy-wait delays
/// @warning These functions block all processing on the current core.
///          Do not use in hot packet-processing paths — prefer
///          deadline-based polling.
/// @{

/**
 * @brief Busy-wait for @p ticks ticks (milliseconds).
 *
 * Spins on the TSC for the full duration (cycle resolution). Works with 
 * interrupts disabled and never changes the interrupt flag.
 */
void timer_sleep_ticks(uint64_t ticks);

/// Busy-wait for @p ms milliseconds (see @c timer_sleep_ticks).
void timer_sleep_ms(uint64_t ms);
/// @}

/// @name Deadline-based timeout helpers
/// @brief Non-blocking timeout checks for polling loops.
///
/// Example:
/// @code{.c}
///   uint64_t deadline = timer_deadline_ms(100); // 100 ms from now
///   while (!device_ready())
///   {
///      if (timer_deadline_expired(deadline))
///      {
///          kprintf("timeout\n");
///          return -1;
///      }
///      cpu_relax();
///   }
/// @endcode
/// @{

/// Create a deadline @p ms milliseconds from now.
static inline uint64_t timer_deadline_ms(uint64_t ms)
{
    return timer_get_ticks() + timer_ms_to_ticks(ms);
}

/// Create a deadline @p ticks ticks from now.
static inline uint64_t timer_deadline_ticks(uint64_t ticks)
{
    return timer_get_ticks() + ticks;
}

/// Check if a deadline has passed.
static inline bool timer_deadline_expired(uint64_t deadline)
{
    return timer_get_ticks() >= deadline;
}
/// @}

#endif // EMMK_TIMER_H

