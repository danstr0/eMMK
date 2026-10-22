/**
 * @file pit.h
 * @brief 8254 PIT channel 2, used only as a calibration reference.
 *
 * Channel 2 counts down at a fixed 1.193182 MHz regardless of CPU
 * speed. Its output is visible as bit 5 of port @c 0x61, so a one-shot
 * countdown can be polled with interrupts disabled. eMMK uses it to
 * calibrate the TSC and the LAPIC timer.
 *
 * @par Usage
 * @code{.c}
 *   pit_oneshot_start(PIT_MS_TO_COUNT(10));
 *   // ... start the counter being calibrated ...
 *   while (!pit_oneshot_done())
 *       cpu_relax();
 *   pit_oneshot_stop();
 * @endcode
 *
 * TODO: revisit when starting the real-hardware update
 */

#ifndef EMMK_PIT_H
#define EMMK_PIT_H

#include "types.h"
#include "io.h"

/// PIT input clock (Hz).
#define PIT_FREQ  1193182UL

/// @name I/O ports
/// @{
#define PIT_CH2_DATA   0x42  ///< Channel 2 count register.
#define PIT_COMMAND    0x43  ///< Mode/command register.

/// Bit 0: ch2 gate; Bit 1: speaker; Bit 5: ch2 out.
#define PIT_GATE_PORT  0x61
/// @}

/// Counter value for @p ms milliseconds (must be ≤ 54 ms).
#define PIT_MS_TO_COUNT(ms)  ((uint16_t)(PIT_FREQ * (ms) / 1000))

/**
 * @brief Start a channel 2 one-shot countdown of @p count PIT cycles.
 *
 * Mode 0 (interrupt on terminal count): the output goes high when the
 * count reaches zero. The speaker is disconnected.
 */
static inline void pit_oneshot_start(uint16_t count)
{
    uint8_t gate = inb(PIT_GATE_PORT);
    outb(PIT_GATE_PORT, (gate & ~0x03) | 0x01);   // gate on, speaker off
    outb(PIT_COMMAND, 0xB0);                      // ch2, lo/hi byte, mode 0
    outb(PIT_CH2_DATA, (uint8_t)(count & 0xFF));
    outb(PIT_CH2_DATA, (uint8_t)(count >> 8));
}

/// @return @c true once the countdown started by @c pit_oneshot_start ends.
static inline bool pit_oneshot_done(void)
{
    return (inb(PIT_GATE_PORT) & 0x20) != 0;
}

/// Close the channel 2 gate.
static inline void pit_oneshot_stop(void)
{
    outb(PIT_GATE_PORT, inb(PIT_GATE_PORT) & ~0x03);
}

#endif // EMMK_PIT_H

