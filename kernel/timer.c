/**
 * @file timer.c
 * @brief Kernel time API implementation (TSC-based, tickless).
 */

#include "timer.h"
#include "serial.h"

// ── Initialization ─────────────────────────────────────────────────────

void timer_init(void)
{
    if (tsc_get_freq() == 0)
        kprintf("[TIMER] ERROR: tsc_init() has not run; clock reads 0\n");

    kprintf("[TIMER] Tickless: TSC clock, %u Hz tick unit, "
            "no periodic interrupt\n", TIMER_HZ);
}

// ── Busy-wait sleeps ───────────────────────────────────────────────────

// Both measure the full duration from the call, at cycle resolution
// (a tick-boundary deadline could expire up to one tick early)

void timer_sleep_ticks(uint64_t ticks)
{
    tsc_delay_us(timer_ticks_to_ms(ticks) * 1000);
}

void timer_sleep_ms(uint64_t ms)
{
    tsc_delay_us(ms * 1000);
}

