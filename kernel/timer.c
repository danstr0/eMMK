/**
 * @file timer.c
 * @brief Timer subsystem implementation.
 *
 * Wraps the APIC timer (periodic mode) and provides tick-based sleep
 * and deadline helpers.
 */

#include "timer.h"
#include "apic.h"
#include "serial.h"

// ── Initialization ─────────────────────────────────────────────────────

void timer_init(void)
{
    kprintf("[TIMER] Initializing timer subsystem...\n");

    // Start the APIC timer at TIMER_HZ
    lapic_timer_init(TIMER_HZ);

    kprintf("[TIMER] Running at %u Hz (%lu us/tick)\n",
            TIMER_HZ, (uint64_t)(1000000 / TIMER_HZ));
}

// ── Tick counter ───────────────────────────────────────────────────────

uint64_t timer_get_ticks(void)
{
    return lapic_timer_get_ticks();
}

// ── Tick-based sleep ───────────────────────────────────────────────────

void timer_sleep_ticks(uint64_t ticks)
{
    uint64_t deadline = timer_deadline_ticks(ticks);

    // sti; hlt is an atomic-ish sequence on x86; the CPU enables
    // interrupts and halts in one step, so it doesn't spin burning
    // power.
    //
    // Note: this re-enables interrupts if they were disabled.
    while (!timer_deadline_expired(deadline))
        __asm__ volatile("sti; hlt");
}

void timer_sleep_ms(uint64_t ms)
{
    uint64_t ticks = timer_ms_to_ticks(ms);
    if (ticks == 0)
        ticks = 1;
    timer_sleep_ticks(ticks);
}

