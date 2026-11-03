/**
 * @file irq.h
 * @brief Interrupt-vector allocator for device interrupts.
 *
 * Device drivers never pick IDT vector numbers themselves; they ask this
 * allocator for one, so any number of devices (and MSI-X entries per
 * device) can coexist. The allocator hands out vectors from the dynamic
 * range @c IRQ_DYN_FIRST – @c IRQ_DYN_LAST and installs the handler in
 * the IDT. Fixed system vectors (exceptions, the legacy PIC range, the
 * APIC timer / error / wake / spurious vectors) lie outside that range.
 *
 * @par Per-vector context
 * Each allocated vector carries an opaque context pointer (typically the
 * device instance). Handlers keep the plain @c interrupt_handler_t
 * signature and recover their context with
 * @c irq_vector_ctx(frame->vector).
 *
 * @par Locking
 * Allocation and release take a spinlock (any core may probe a device).
 * @c irq_vector_ctx() is lock-free: it is a single load, and the context
 * is published before the handler is installed.
 */

#ifndef EMMK_IRQ_H
#define EMMK_IRQ_H

#include "types.h"
#include "idt.h"

/// @name Dynamic vector range
/// @{

/// First vector handed out by @c irq_alloc_vector().
#define IRQ_DYN_FIRST  64

/// Last vector handed out by @c irq_alloc_vector() (@c 0xEF; the wake
/// vector @c 0xF0 follows)
#define IRQ_DYN_LAST   239

/// Number of vectors in the dynamic range.
#define IRQ_DYN_COUNT  (IRQ_DYN_LAST - IRQ_DYN_FIRST + 1)
/// @}

/**
 * @brief Allocate a free vector and install @p handler for it.
 *
 * Vectors are handed out lowest-first. Note: the LAPIC prioritizes
 * interrupts by vector (higher = higher priority, in classes of 16).
 *
 * @param handler  Handler to install (must not be @c NULL).
 * @param ctx      Opaque context, returned by @c irq_vector_ctx().
 * @return The vector (@c IRQ_DYN_FIRST – @c IRQ_DYN_LAST), or @c -1 if
 *         the range is exhausted or @p handler is @c NULL.
 */
int irq_alloc_vector(interrupt_handler_t handler, void *ctx);

/**
 * @brief Release a vector obtained from @c irq_alloc_vector().
 *
 * Uninstalls the handler. The caller must first make sure the device
 * can no longer raise the vector (e.g., mask its MSI-X entry). Vectors
 * outside the dynamic range, or not allocated, are ignored.
 *
 * @param vector  Vector to release.
 */
void irq_free_vector(int vector);

/**
 * @brief Context pointer registered with an allocated vector.
 *
 * @param vector  Vector number (typically @c frame->vector).
 * @return The @p ctx passed to @c irq_alloc_vector(), or @c NULL if the
 *         vector is not allocated.
 */
void *irq_vector_ctx(uint64_t vector);

/// @return Number of vectors still free in the dynamic range.
int irq_vectors_free(void);

#endif // EMMK_IRQ_H

