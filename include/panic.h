/**
 * @file panic.h
 * @brief System-wide kernel panic.
 *
 * A panic stops the whole machine, not just the core that hit it: on a
 * multi-core packet processor, a crashed worker would otherwise leave
 * the rest of the pipeline running on corrupted state while its own
 * report interleaves with their output.
 *
 * @par Sequence
 * 1. The first core to panic claims ownership. Any other core that panics
 *    concurrently halts silently.
 * 2. The console switches to lock-free panic mode
 *    (@c serial_panic_enter()).
 * 3. An NMI is broadcast to all other cores (once the LAPIC is up). Their
 *    NMI handler sees the panic in progress and halts — NMIs are
 *    delivered even with interrupts disabled.
 * 4. The owner prints its report and halts.
 *
 * A fault while printing the report (a nested panic on the owning core)
 * halts immediately.
 */

#ifndef EMMK_PANIC_H
#define EMMK_PANIC_H

#include "types.h"
#include "idt.h"

/**
 * @brief Halt the system with a formatted message.
 *
 * Use for unrecoverable software conditions (out of memory during init,
 * corrupted invariants). Safe from any context.
 */
PRINTF_FMT(1, 2)
NORETURN void panic(const char *fmt, ...);

/**
 * @brief Halt the system with a full report for a CPU exception.
 *
 * Called by @c interrupt_dispatch() for exceptions with no registered
 * handler. Prints the execution name, error code, all registers, and
 * page-fault details.
 */
NORETURN void panic_exception(struct interrupt_frame *frame);

/**
 * @brief NMI (vector 2) handler, installed by @c idt_init();
 *
 * During a panic, halts the receiving core. Otherwise an NMI is
 * unexpected (eMMK sends none outside panics; on hardware they signal
 * errors or watchdogs) and itself causes a panic.
 */
void panic_nmi_handler(struct interrupt_frame *frame);

/// @return @c true once any core has begun panicking.
bool panic_in_progress(void);

#endif // EMMK_PANIC_H

