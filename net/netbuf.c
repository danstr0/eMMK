/**
 * @file netbuf.c
 * @brief Network buffer allocation, pools, and queue management.
 *
 * A netbuf is one allocation: metadata at the front, then the device
 * headroom, then the data region. It comes from one of:
 * - a per-queue pool (received packets; no locks on the owner core),
 * - @c netbuf_cache, a slab of @c NETBUF_SIZE objects (locally built
 *   packets; any core),
 * - the PMM, for buffers larger than @c NETBUF_DATA_SIZE.
 *
 * The netbuf subsystem must be initialized via @c netbuf_init() before
 * any allocations.
 */

#include "netbuf.h"
#include "slab.h"
#include "pmm.h"
#include "smp.h"
#include "string.h"
#include "serial.h"

// ── State ──────────────────────────────────────────────────────────────

/// Cache for standard-size netbufs (metadata and data in one object).
static struct slab_cache *netbuf_cache;

/// Slab / PMM allocations (atomic: any core). Stays flat on the data path.
static uint64_t slow_allocs;

// ── Initialization ─────────────────────────────────────────────────────

void netbuf_init(void)
{
    netbuf_cache = slab_cache_create("netbuf", NETBUF_SIZE, SLAB_NONE);

    if (!netbuf_cache)
        kprintf("[NETBUF] ERROR: Failed to create slab cache\n");
    else
        kprintf("[NETBUF] Initialized (metadata=%lu bytes, data=%u bytes)\n",
                (uint64_t)sizeof(struct netbuf), NETBUF_DATA_SIZE);
}

// ── Allocation / deallocation ──────────────────────────────────────────

/**
 * @brief Reset the metadata of the buffer at @p nb.
 *
 * Only metadata is touched: there is no per-packet @c memset of the data.
 *
 * @param nb    Start of the buffer.
 * @param size  Bytes between @c head and @c end.
 * @param phys  Physical address of the start of the buffer.
 * @param pool  Owning pool, or @c NULL.
 */
static inline void netbuf_reset(struct netbuf *nb, size_t size,
                                uint64_t phys, struct netbuf_pool *pool)
{
    uint8_t *head = (uint8_t *)nb + NETBUF_DATA_OFFSET;

    memset(nb, 0, sizeof(*nb));
    nb->head      = head;
    nb->data      = head;
    nb->tail      = head;
    nb->end       = head + size;
    nb->phys_addr = phys + NETBUF_DATA_OFFSET;
    nb->pool      = pool;
    nb->refcount  = 1;
}

struct netbuf *netbuf_alloc(size_t size)
{
    if (size == 0)
        size = NETBUF_DATA_SIZE;

    struct netbuf *nb;
    if (size <= NETBUF_DATA_SIZE)
    {
        nb = slab_alloc(netbuf_cache);
        if (!nb)
            return NULL;
        netbuf_reset(nb, size, VIRT_TO_PHYS_ADDR((uintptr_t)nb), NULL);
    }
    else
    {
        // Oversized buffer; fall back to the page allocator
        size_t pages = PAGE_ALIGN_UP(size + NETBUF_DATA_OFFSET) / PAGE_SIZE;
        uint64_t phys = pmm_alloc_pages(pages);
        if (phys == PMM_ALLOC_FAILED)
            return NULL;
        nb = (struct netbuf *)PHYS_TO_VIRT(phys);
        netbuf_reset(nb, size, phys, NULL);
    }

    __atomic_fetch_add(&slow_allocs, 1, __ATOMIC_RELAXED);
    return nb;
}

uint64_t netbuf_slow_allocs(void)
{
    return __atomic_load_n(&slow_allocs, __ATOMIC_RELAXED);
}

// ── Pools ──────────────────────────────────────────────────────────────

struct netbuf_pool *netbuf_pool_create(int owner_core)
{
    size_t pages = NETBUF_POOL_BLOCK_SIZE / PAGE_SIZE;
    uint64_t phys = pmm_alloc_aligned(pages, pages);
    if (phys == PMM_ALLOC_FAILED)
    {
        kprintf("[NETBUF] No free 2 MiB block for a pool\n");
        return NULL;
    }

    // The pool structure occupies the first slots of its own block
    struct netbuf_pool *pool = (struct netbuf_pool *)PHYS_TO_VIRT(phys);
    size_t first = ALIGN_UP(sizeof(struct netbuf_pool), NETBUF_SIZE);

    pool->owner_core   = owner_core;
    pool->capacity     = (NETBUF_POOL_BLOCK_SIZE - first) / NETBUF_SIZE;
    pool->free_count   = 0;
    pool->phys         = phys;
    pool->remote_free  = NULL;
    pool->allocs       = 0;
    pool->alloc_fails  = 0;
    pool->remote_frees = 0;

    // Stacked so that the lowest addresses are handed out first
    for (uint32_t i = pool->capacity; i > 0; --i)
    {
        size_t off = first + (size_t)(i - 1) * NETBUF_SIZE;
        struct netbuf *nb = (struct netbuf *)((uint8_t *)pool + off);
        netbuf_reset(nb, NETBUF_DATA_SIZE, phys + off, pool);
        nb->refcount = 0;
        pool->stack[pool->free_count++] = nb;
    }

    return pool;
}

void netbuf_pool_destroy(struct netbuf_pool *pool)
{
    if (pool)
        pmm_free_pages(pool->phys, NETBUF_POOL_BLOCK_SIZE / PAGE_SIZE);
}

/// Move everything other cores have freed onto the owner's stack.
static void pool_reclaim_remote(struct netbuf_pool *pool)
{
    // Taking the whole list rules out ABA: nodes are never popped singly
    struct netbuf *nb = __atomic_exchange_n(&pool->remote_free, NULL,
                                            __ATOMIC_ACQUIRE);
    while (nb)
    {
        struct netbuf *next = nb->next;
        pool->stack[pool->free_count++] = nb;
        nb = next;
    }
}

struct netbuf *netbuf_pool_alloc(struct netbuf_pool *pool)
{
    if (UNLIKELY(pool->free_count == 0))
    {
        pool_reclaim_remote(pool);
        if (pool->free_count == 0)
        {
            pool->alloc_fails++;
            return NULL;
        }
    }

    struct netbuf *nb = pool->stack[--pool->free_count];
    uint64_t phys = nb->phys_addr - NETBUF_DATA_OFFSET;
    netbuf_reset(nb, NETBUF_DATA_SIZE, phys, pool);
    pool->allocs++;
    return nb;
}

uint32_t netbuf_pool_available(struct netbuf_pool *pool)
{
    pool_reclaim_remote(pool);
    return pool->free_count;
}

/// Return a buffer whose last reference is gone to its pool.
static inline void pool_free(struct netbuf_pool *pool, struct netbuf *nb)
{
    if (LIKELY(pool->owner_core == smp_core_id()))
    {
        pool->stack[pool->free_count++] = nb;
        return;
    }

    // Another core: push on the owner's list
    __atomic_fetch_add(&pool->remote_frees, 1, __ATOMIC_RELAXED);
    struct netbuf *head = __atomic_load_n(&pool->remote_free, __ATOMIC_RELAXED);
    do
        nb->next = head;
    while (!__atomic_compare_exchange_n(&pool->remote_free, &head, nb, true,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED));
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

    if (nb->pool)
    {
        pool_free(nb->pool, nb);
        return;
    }

    // Slab or PMM: told apart by the buffer's size
    size_t size = (size_t)(nb->end - nb->head);

    if (size <= NETBUF_DATA_SIZE)
        slab_free(netbuf_cache, nb);
    else
    {
        size_t pages = PAGE_ALIGN_UP(size + NETBUF_DATA_OFFSET) / PAGE_SIZE;
        pmm_free_pages(nb->phys_addr - NETBUF_DATA_OFFSET, pages);
    }
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

