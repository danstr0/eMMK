/**
 * @file spsc.h
 * @brief Lock-free Single-Producer Single-Consumer ring buffer.
 *
 * A fixed-size, power-of-2 ring that allows one producer and one
 * consumer to operate concurrently without locks or atomic
 * instructions on the hot path.
 *
 * @par Memory ordering (x86-64)
 * On x86-64, stores are not reordered with other stores and loads
 * are not reordered with other loads. The @c volatile qualifiers on
 * @c head and @c tail provide a compiler barrier; no @c MFENCE is
 * needed. This makes enqueue and dequeue zero-overhead beyond the
 * actual data movement.
 *
 * @par Cache-line separation
 * @c head (written by the producer) and @c tail (written by the
 * consumer) are placed on separate 64-byte cache lines to prevent
 * false sharing. Without this, every enqueue would invalidate the
 * consumer's cache line and vice versa, destroying throughput.
 *
 * @par Usage
 * @code{.c}
 *   // Producer (distributor core):
 *   if (!spsc_ring_enqueue(ring, nb))
 *       handle_drop(nb);
 *
 *   // Consumer (worker core):
 *   void *ptr;
 *   while (spsc_ring_dequeue(ring, &ptr))
 *       process((struct netbuf *)ptr);
 * @endcode
 */

#ifndef EMMK_SPSC_H
#define EMMK_SPSC_H

#include "types.h"

/// Default ring size (@b must be a power of 2).
#define SPSC_RING_SIZE_DEFAULT  256

/// @name Ring structure
/// @{

/**
 * @brief SPSC ring buffer.
 *
 * @c head and @c tail are monotonically increasing indices. The
 * actual slot is obtained via <code>index & mask</code>. This avoids
 * ambiguity between full and empty.
 *
 * The ring is full when <code>head - tail == size</code>.
 * The ring is empty when <code>head == tail</code>.
 */
struct spsc_ring
{
    void   **entries;  ///< Pointer array (power-of-2 elements).
    uint32_t size;     ///< Number of slots (power of 2).
    uint32_t mask;     ///< <code>size - 1</code> (for fast modulo).

    /// Producer index — only written by the enqueue side.
    volatile uint32_t head ALIGNED(64);

    /// Consumer index — only written by the dequeue side.
    volatile uint32_t tail ALIGNED(64);
};
/// @}

/// @name Lifecycle
/// @{

/**
 * @brief Initialize a ring with a caller-provided buffer.
 *
 * @param ring    Ring to initialize.
 * @param buffer  Pointer array with @p size elements.
 * @param size    Number of slots (must be a power of 2).
 */
void spsc_ring_init(struct spsc_ring *ring, void **buffer, uint32_t size);

/**
 * @brief Allocate and initialize a ring.
 *
 * @param size  Number of slots (must be a power of 2; @c 0 = default).
 * @return Allocated ring, or @c NULL on failure.
 */
struct spsc_ring *spsc_ring_create(uint32_t size);

/// Free a ring created by @c spsc_ring_create().
void spsc_ring_destroy(struct spsc_ring *ring);
/// @}

/// @name Hot-path operations (inline)
/// @{

/**
 * @brief Enqueue a pointer (producer side).
 *
 * @param ring  Ring buffer.
 * @param ptr   Pointer to enqueue (must not be @c NULL).
 * @return @c true on success, @c false if the ring is full.
 */
static inline bool spsc_ring_enqueue(struct spsc_ring *ring, void *ptr)
{
    uint32_t head = ring->head;
    uint32_t tail = ring->tail;

    if (head - tail >= ring->size)
        return false;  // full

    ring->entries[head & ring->mask] = ptr;
    barrier();
    ring->head = head + 1;

    return true;
}

/**
 * @brief Dequeue a pointer (consumer side).
 *
 * @param ring      Ring buffer.
 * @param[out] ptr  Receives the dequeued pointer.
 * @return @c true on success, @c false if the ring is empty.
 */
static inline bool spsc_ring_dequeue(struct spsc_ring *ring, void **ptr)
{
    uint32_t tail = ring->tail;
    uint32_t head = ring->head;

    if (tail == head)
        return false; // empty

    *ptr = ring->entries[tail & ring->mask];
    barrier();
    ring->tail = tail + 1;

    return true;
}

/**
 * @brief Dequeue up to @p max pointers in a batch.
 *
 * @param ring  Ring buffer.
 * @param out   Output array (must hold @p max elements).
 * @param max   Maximum number to dequeue.
 * @return Number of pointers actually dequeued.
 */
static inline uint32_t spsc_ring_dequeue_batch(struct spsc_ring *ring,
                                               void **out, uint32_t max)
{
    uint32_t tail = ring->tail;
    uint32_t head = ring->head;
    uint32_t avail = head - tail;

    if (avail > max)
        avail = max;

    for (uint32_t i = 0; i < avail; ++i)
        out[i] = ring->entries[(tail + i) & ring->mask];

    barrier();
    ring->tail = tail + avail;

    return avail;
}
/// @}

/// @name Queries (safe from either side)
/// @{

/// Number of entries currently in the ring.
static inline uint32_t spsc_ring_count(struct spsc_ring *ring)
{
    return ring->head - ring->tail;
}

/// @c true if the ring contains no entries.
static inline bool spsc_ring_empty(struct spsc_ring *ring)
{
    return ring->head == ring->tail;
}

/// @c true if the ring is at capacity.
static inline bool spsc_ring_full(struct spsc_ring *ring)
{
    return (ring->head - ring->tail) >= ring->size;
}
/// @}

#endif // EMMK_SPSC_H

