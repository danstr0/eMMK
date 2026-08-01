/**
 * @file spsc.c
 * @brief SPSC ring buffer allocation.
 */

#include "spsc.h"
#include "heap.h"
#include "string.h"
#include "serial.h"

void spsc_ring_init(struct spsc_ring *ring, void **buffer, uint32_t size)
{
    ring->entries = buffer;
    ring->size    = size;
    ring->mask    = size - 1;
    ring->head    = 0;
    ring->tail    = 0;
}

struct spsc_ring *spsc_ring_create(uint32_t size)
{
    if (size == 0)
        size = SPSC_RING_SIZE_DEFAULT;

    // Enforce power of 2
    if (size & (size - 1))
    {
        kprintf("[SPSC] ERROR: size %u is not a power of 2\n", size);
        return NULL;
    }

    struct spsc_ring *ring = kzalloc(sizeof(struct spsc_ring));
    if (!ring)
        return NULL;

    ring->entries = kzalloc(size * sizeof(void *));
    if (!ring->entries)
    {
        kfree(ring);
        return NULL;
    }

    ring->size = size;
    ring->mask = size - 1;
    ring->head = 0;
    ring->tail = 0;

    return ring;
}

void spsc_ring_destroy(struct spsc_ring *ring)
{
    if (!ring)
        return;
    if (ring->entries)
        kfree(ring->entries);
    kfree(ring);
}

