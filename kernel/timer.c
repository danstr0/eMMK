/**
 * @file timer.c
 * @brief Timer subsystem implementation.
 *
 * Wraps the APIC timer (periodic mode) and provides tick-based sleep,
 * TSC-based microsecond delay, and deadline helpers.
 *
 * @pre Interrupts must be enabled before @c timer_init() is called,
 *      because the TSC calibration routine waits for APIC timer ticks.
 */

#include "timer.h"
#include "apic.h"
#include "serial.h"

// ── TSC (Time Stamp Counter) helpers ───────────────────────────────────────

/// Read the 64-bit Time Stamp Counter.
static inline uint64_t rdtsc(void)
{
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

/**
 * Estimated TSC ticks per microsecond.
 *
 * Calibrated once during @c timer_init() by measuring TSC ticks over
 * a known APIC timer interval.
 */
static uint64_t tsc_ticks_per_us;

/**
 * @brief Measure TSC frequency using the APIC timer as reference.
 *
 * Requires the APIC timer to be running and interrupts to be enabled.
 */
static void calibrate_tsc(void)
{
    // Wait for a tick boundary to get a clean measurement
    uint64_t start_tick = lapic_timer_get_ticks();
    while (lapic_timer_get_ticks() == start_tick)
        __asm__ volatile("pause");

    // Measure TSC ticks over one full timer tick
    uint64_t tsc_start = rdtsc();
    start_tick = lapic_timer_get_ticks();
    while (lapic_timer_get_ticks() == start_tick)
        __asm__ volatile("pause");
    uint64_t tsc_end = rdtsc();

    // One tick = 1000000/TIMER_HZ microseconds
    uint64_t tsc_per_tick = tsc_end - tsc_start;
    uint64_t us_per_tick  = 1000000 / TIMER_HZ;

    tsc_ticks_per_us = tsc_per_tick / us_per_tick;
    if (tsc_ticks_per_us == 0)
        tsc_ticks_per_us = 1;

    kprintf("[TIMER] TSC calibration: ~%lu ticks/us (~%lu MHz)\n",
            tsc_ticks_per_us, tsc_ticks_per_us);
}

// ── Initialization ─────────────────────────────────────────────────────

void timer_init(void)
{
    kprintf("[TIMER] Initializing timer subsystem...\n");

    // Start the APIC timer at TIMER_HZ
    lapic_timer_init(TIMER_HZ);

    // Enable interrupts so the APIC timer can fire during TSC
    // calibration. They must remain enabled for tick-based functions
    // to work.
    __asm__ volatile("sti");

    // Calibrate TSC for microsecond-precision delays
    calibrate_tsc();

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

// ── TSC-based busy-wait delay ──────────────────────────────────────────

void timer_delay_us(uint64_t us)
{
    uint64_t target = rdtsc() + (us * tsc_ticks_per_us);
    while (rdtsc() < target)
        cpu_relax();
}

