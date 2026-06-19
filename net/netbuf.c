/**
 * @file netbuf.c
 * @brief Network buffer allocation and queue management.
 *
 * Uses two slab caches for fast allocation:
 * - @c netbuf_cache      - @c struct netbuf metadata (zeroed).
 * - @c netbuf_data_cache - 2 KiB data buffers (standard-size packets).
 *
 * For buffers larger than @c NETBUF_SIZE, the data region is allocated
 * via the PMM (page allocator) instead of the slab.
 *
 * The netbuf subsystem must be initialized via @c netbuf_init() before
 * any allocations.
 */

#include "netbuf.h"
#include "slab.h"
#include "pmm.h"
#include "string.h"
#include "serial.h"

// ── Slab caches ────────────────────────────────────────────────────────

/// Cache for @c struct @c netbuf metadata. @c SLAB_ZERO ensures clean state.
static struct slab_cache *netbuf_cache;

/// Cache for standard-size (@c NETBUF_SIZE) data buffers.
static struct slab_cache *netbuf_data_cache;

// ── Initialization ─────────────────────────────────────────────────────

void netbuf_init(void)
{
    netbuf_cache = slab_cache_create("netbuf",
                                     sizeof(struct netbuf), SLAB_ZERO);
    netbuf_data_cache = slab_cache_create("netbuf_data",
                                          NETBUF_SIZE, SLAB_NONE);

    if (!netbuf_cache || !netbuf_data_cache)
        kprintf("[NETBUF] ERROR: Failed to create slab caches\n");
    else
        kprintf("[NETBUF] Initialized (struct=%lu bytes, data=%u bytes)\n",
                (uint64_t)sizeof(struct netbuf), NETBUF_SIZE);
}

// ── Allocation / deallocation ──────────────────────────────────────────

struct netbuf *netbuf_alloc(size_t size)
{
    if (size == 0)
        size = NETBUF_SIZE;

    struct netbuf *nb = slab_alloc(netbuf_cache);
    if (!nb)
        return NULL;

    // Allocate the data buffer
    uint8_t *data;
    if (size <= NETBUF_SIZE)
    {
        data = slab_alloc(netbuf_data_cache);
        if (!data)
        {
            slab_free(netbuf_cache, nb);
            return NULL;
        }
        nb->phys_addr = VIRT_TO_PHYS_ADDR((uintptr_t)data);
    }
    else
    {
        // Oversized buffer — fall back to the page allocator
        size_t pages = PAGE_ALIGN_UP(size) / PAGE_SIZE;
        uint64_t phys = pmm_alloc_pages(pages);
        if (phys == PMM_ALLOC_FAILED)
        {
            slab_free(netbuf_cache, nb);
            return NULL;
        }
        data = (uint8_t *)PHYS_TO_VIRT(phys);
        nb->phys_addr = phys;
    }

    // Initialize buffer pointers (empty - no data yet)
    nb->head = data;
    nb->data = data;
    nb->tail = data;
    nb->end  = data + size;

    nb->len      = 0;
    nb->refcount = 1;
    nb->next     = NULL;

    return nb;
}

void netbuf_free(struct netbuf *nb)
{
    if (!nb)
        return;

    if (nb->refcount <= 0)
    {
        kprintf("[NETBUF] WARNING: free with refcount %d at %p\n",
                nb->refcount, (void *)nb);
        return;
    }

    nb->refcount--;
    if (nb->refcount > 0)
        return;

    // Determine how to free the data based on its size
    size_t size = (size_t)(nb->end - nb->head);

    if (size <= NETBUF_SIZE)
        slab_free(netbuf_data_cache, nb->head);
    else
    {
        size_t pages = PAGE_ALIGN_UP(size) / PAGE_SIZE;
        pmm_free_pages(nb->phys_addr, pages);
    }

    slab_free(netbuf_cache, nb);
}

// ── Copy ───────────────────────────────────────────────────────────────

/**
 * @brief Recalculate a header pointer from the source buffer into the
 *        destination buffer.
 *
 * @param ptr       Header pointer in the source buffer (may be @c NULL).
 * @param src_head  Source buffer's @c head.
 * @param dst_head  Destination buffer's @c head.
 * @return Equivalent pointer in the destination, or @c NULL.
 */
static inline void *remap_header_ptr(void *ptr, uint8_t *src_head,
                                     uint8_t *dst_head)
{
    if (!ptr)
        return NULL;
    return dst_head + ((uint8_t *)ptr - src_head);
}

struct netbuf *netbuf_copy(struct netbuf *nb)
{
    if (!nb)
        return NULL;

    size_t total = (size_t)(nb->end - nb->head);
    struct netbuf *copy = netbuf_alloc(total);
    if (!copy)
        return NULL;

    // Reserve the same headroom as the source
    size_t headroom = (size_t)(nb->data - nb->head);
    netbuf_reserve(copy, headroom);

    // Copy the data region
    void *dst = netbuf_put(copy, nb->len);
    if (!dst)
    {
        netbuf_free(copy);
        return NULL;
    }
    memcpy(dst, nb->data, nb->len);

    // Copy scalar metadata
    copy->protocol = nb->protocol;
    copy->pkt_type = nb->pkt_type;
    copy->ip_proto = nb->ip_proto;
    copy->src_ip   = nb->src_ip;
    copy->dst_ip   = nb->dst_ip;
    copy->src_port = nb->src_port;
    copy->dst_port = nb->dst_port;
    copy->dev      = nb->dev;

    // Remap header pointers from source buffer into copy buffer
    copy->mac_header       = remap_header_ptr(nb->mac_header,
                                              nb->head, copy->head);
    copy->network_header   = remap_header_ptr(nb->network_header,
                                              nb->head, copy->head);
    copy->transport_header = remap_header_ptr(nb->transport_header,
                                              nb->head, copy->head);
    return copy;
}

// ── Packet queue ───────────────────────────────────────────────────────

void netbuf_queue_push(struct netbuf_queue *q, struct netbuf *nb)
{
    nb->next = NULL;
    if (q->tail)
        q->tail->next = nb;
    else
        q->head = nb;
    q->tail = nb;
    q->count++;
}

struct netbuf *netbuf_queue_pop(struct netbuf_queue *q)
{
    struct netbuf *nb = q->head;
    if (!nb)
        return NULL;

    q->head = nb->next;
    if (!q->head)
        q->tail = NULL;
    nb->next = NULL;
    q->count--;

    return nb;
}

void netbuf_queue_purge(struct netbuf_queue *q)
{
    struct netbuf *nb;
    while ((nb = netbuf_queue_pop(q)) != NULL)
        netbuf_free(nb);
}

