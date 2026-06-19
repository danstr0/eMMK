/**
 * @file timer.h
 * @brief Timer subsystem interface.
 *
 * Provides:
 * - System tick counter driven by the APIC timer.
 * - Monotonic time conversions (ticks → ms, seconds).
 * - Busy-wait delay functions (ms, µs).
 * - Deadline-based timeout helpers for driver polling loops.
 *
 * The timer runs at @c TIMER_HZ ticks per second (default 1000 = 1 ms
 * resolution). Each tick increments a global counter via the APIC
 * timer interrupt handler.
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

/// @name Configuration
/// @{

/**
 * @brief Timer interrupt frequency.
 *
 * 1000 Hz = 1 ms tick resolution.
 *
 * @note Higher values give finer granularity at the cost of more
 *       interrupt overhead per core.
 */
#define TIMER_HZ  1000
/// @}

/// @name Initialization
/// @{

/**
 * @brief Initialize the timer subsystem.
 *
 * Configures the APIC timer for periodic interrupts at @c TIMER_HZ
 * and calibrates the TSC for microsecond-precision delays.
 *
 * @pre @c lapic_init() and @c idt_init() must have been called.
 * @pre Interrupts must be enabled (the calibration routine waits for
 *      APIC timer ticks).
 */
void timer_init(void);
/// @}

/// @name Tick counter
/// @{

/**
 * @brief Read the monotonic tick counter.
 *
 * @return Number of timer interrupts since @c timer_init() was called.
 *
 * @note Wraps after ~584 million years at 1000 Hz (@c uint64_t).
 */
uint64_t timer_get_ticks(void);
/// @}

/// @name Time conversions
/// @{

/**
 * @brief Milliseconds elapsed since boot.
 *
 * For @c TIMER_HZ = 1000, this is identical to @c timer_get_ticks().
 */
static inline uint64_t timer_get_ms(void)
{
    return timer_get_ticks() * 1000 / TIMER_HZ;
}

/// Seconds since boot (truncated).
static inline uint64_t timer_get_uptime(void)
{
    return timer_get_ticks() / TIMER_HZ;
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
 * @brief Busy-wait for @p ticks timer interrupts.
 *
 * @param ticks  Number of ticks to wait.
 *
 * @note Requires interrupts to be enabled — the tick counter is updated
 *       by the APIC timer interrupt handler. This function re-enables
 *       interrupts via @c sti if they are currently disabled.
 */
void timer_sleep_ticks(uint64_t ticks);

/**
 * @brief Busy-wait for @p ms milliseconds.
 *
 * @note Requires interrupts to be enabled.
 */
void timer_sleep_ms(uint64_t ms);

/**
 * @brief Busy-wait for @p us microseconds
 *
 * Uses a TSC-based spin loop — works even with interrupts disabled.
 * Precision depends on TSC calibration quality.
 */
void timer_delay_us(uint64_t us);
/// @}

/// @name Deadline-based timeout helpers
/// @brief Non-blocking timeout checks for polling loops.
///
/// Example:
/// @code
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

