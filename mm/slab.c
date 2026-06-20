/**
 * @file slab.c
 * @brief Slab allocator.
 *
 * Each slab is one or more contiguous physical pages, laid out as:
 * @code
 *   [slab_header] [alignment pad] [obj][obj][obj]...[obj]
 * @endcode
 *
 * Free objects form an in-object linked list (the first @c sizeof(void*)
 * bytes of each free object store a pointer to the next free object).
 *
 * Slabs are organized into three lists per cache:
 * - @b partial : has both free and allocated objects (checked first).
 * - @b full    : all objects allocated (no free objects).
 * - @b empty   : all objects free (returned to PMM by
 *                  @c slab_cache_shrink)
 *
 * Cache metadata is heap-allocated (via @c kzalloc). Slab pages are
 * PMM-allocated and accessed through the higher-half direct mapping.
 *
 * @note This relies on all PMM-allocated pages being accessible via
 *       @c PHYS_TO_VIRT(), which is true as long as the PMM is limited
 *       to the first 1 GiB (covered by the boot-time mapping). If
 *       @c PMM_MAX_MEMORY is increased, slabs must be explicitly mapped
 *       via the VMM.
 */

#include "slab.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "serial.h"
#include "string.h"
#include "spinlock.h"
#include "smp.h"
#include "percpu.h"

// ── Internal structures ────────────────────────────────────────────────

/**
 * @brief Header stored at the start of each slab (page-group).
 *
 * The @c free_list is an in-object linked list: each free object's
 * first @c sizeof(void*) bytes point to the next free object (or
 * @c NULL).
 */
struct slab
{
    struct slab       *next;
    struct slab       *prev;
    struct slab_cache *cache;      ///< Owning cache (for validation).
    void              *free_list;  ///< Head of in-object free list.
    size_t            in_use;      ///< Number of allocated objects.
    size_t            total;       ///< Total object slots in this slab.
    bool              was_full;    ///< Tracks which list this slab is on.
};

/// @name Per-core magazine
/// @{

/**
 * Each core keeps a small local free list ("magazine") per cache.
 * Alloc pops from the local list (no lock); free pushes onto it.
 * When the magazine is empty, a batch is refilled from the shared
 * cache (under the lock). When it overflows, a batch is returned.
 */
#define SLAB_MAGAZINE_SIZE  32

struct slab_magazine
{
    void *objects[SLAB_MAGAZINE_SIZE];
    int   count;                  ///< Number of objects in the magazine.
};
/// @}

/// All metadata for a single object-size class.
struct slab_cache
{
    char     name[SLAB_NAME_MAX];
    size_t   obj_size;              ///< Effective object size (aligned).
    size_t   obj_align;             ///< Object alignment.
    size_t   requested_size;        ///< Original size requested by caller.
    size_t   slab_size;             ///< Total slab size in bytes (power of two).
    size_t   objs_per_slab;         ///< Objects per slab.
    uint32_t flags;

    struct slab *slabs_partial;     ///< Has some free objects.
    struct slab *slabs_full;        ///< All objects allocated.
    struct slab *slabs_empty;       ///< All objects are free.

    size_t total_slabs;
    size_t total_objects;           ///< Total slots across all slabs.
    size_t active_objects;          ///< Currently allocated objects.

    struct slab_cache *next_cache;  ///< Global cache list linkage.

    /// Protects slab lists, counters, and slab creation/destruction.
    /// Not needed for per-core magazine access.
    spinlock_t lock;

    /// Per-core magazines (indexed by core number).
    struct slab_magazine magazines[SMP_MAX_CORES];
};

// ── Global state ───────────────────────────────────────────────────────

/// Linked list of all caches (for diagnostics / reclamation).
static struct slab_cache *all_caches;

/// Minimum object size (must hold a free-list pointer).
#define MIN_OBJ_SIZE  sizeof(void *)

// Forward declarations for locked helpers (used by destroy)
static void *slab_alloc_locked(struct slab_cache *cache);
static void  slab_free_locked(struct slab_cache *cache, void *obj);

// ── Pre-defined size-class caches ──────────────────────────────────────

struct slab_cache *slab_cache_16;
struct slab_cache *slab_cache_32;
struct slab_cache *slab_cache_64;
struct slab_cache *slab_cache_128;
struct slab_cache *slab_cache_256;
struct slab_cache *slab_cache_512;
struct slab_cache *slab_cache_1024;
struct slab_cache *slab_cache_2048;
struct slab_cache *slab_cache_4096;

// ── Cache parameter calculation ────────────────────────────────────────

/**
 * @brief Compute slab dimensions from the requested object size.
 *
 * @pre After this function returns, @c cache->slab_size is guaranteed
 *      to be a power of two. @c slab_free() relies on this invariant
 *      to locate the slab header via address masking.
 */
static void cache_calculate(struct slab_cache *cache)
{
    // Enforce minimum (must fit a free-list pointer)
    if (cache->obj_size < MIN_OBJ_SIZE)
        cache->obj_size = MIN_OBJ_SIZE;

    // Apply alignment
    cache->obj_align = SLAB_MIN_ALIGN;
    cache->obj_size = ALIGN_UP(cache->obj_size, cache->obj_align);

    // Select slab size: target ~32 objects, minimum 1 page, max 8 pages
    //   - Always a power of two
    size_t desired = sizeof(struct slab) + cache->obj_size * 32;
    cache->slab_size = PAGE_SIZE;
    while (cache->slab_size < desired && cache->slab_size < 8 * PAGE_SIZE)
        cache->slab_size *= 2;

    // Calculate objects per slab, accounting for the alignment padding
    // between the slab header and the first object.
    size_t header_end = ALIGN_UP(sizeof(struct slab), cache->obj_align);
    size_t available = cache->slab_size - header_end;
    cache->objs_per_slab = available / cache->obj_size;

    if (cache->objs_per_slab == 0)
    {
        cache->slab_size = PAGE_ALIGN_UP(sizeof(struct slab) + cache->obj_size);

        // Round up to power of two
        size_t s = PAGE_SIZE;
        while (s < cache->slab_size)
            s *= 2;
        cache->slab_size     = s;
        cache->objs_per_slab = 1;
    }
}

// ── Slab list helpers ──────────────────────────────────────────────────

static void slab_list_remove(struct slab **list, struct slab *slab)
{
    if (slab->prev)
        slab->prev->next = slab->next;
    else
        *list = slab->next;

    if (slab->next)
        slab->next->prev = slab->prev;

    slab->next = NULL;
    slab->prev = NULL;
}

static void slab_list_add(struct slab **list, struct slab *slab)
{
    slab->next = *list;
    slab->prev = NULL;
    if (*list)
        (*list)->prev = slab;
    *list = slab;
}

// ── Slab creation / destruction ────────────────────────────────────────

/// Allocate and initialize a new slab for @p cache.
static struct slab *slab_create(struct slab_cache *cache)
{
    size_t pages = cache->slab_size / PAGE_SIZE;
    uint64_t paddr = pmm_alloc_aligned(pages, pages);
    if (paddr == PMM_ALLOC_FAILED)
        return NULL;

    struct slab *slab = (struct slab *)PHYS_TO_VIRT(paddr);

    // Initialize the slab header
    slab->next     = NULL;
    slab->prev     = NULL;
    slab->cache    = cache;
    slab->in_use   = 0;
    slab->total    = cache->objs_per_slab;
    slab->was_full = false;

    // Locate the first object slot
    uint8_t *obj_start = (uint8_t *)slab
                         + ALIGN_UP(sizeof(struct slab), cache->obj_align);

    // Build the in-object free list
    slab->free_list = obj_start;
    for (size_t i = 0; i < cache->objs_per_slab - 1; ++i)
    {
        uint8_t *current = obj_start + i * cache->obj_size;
        uint8_t *next    = current + cache->obj_size;
        *(void **)current = next;
    }
    uint8_t *last = obj_start + (cache->objs_per_slab - 1) * cache->obj_size;
    *(void **)last = NULL;

    cache->total_slabs++;
    cache->total_objects += cache->objs_per_slab;

    return slab;
}

/// Free all pages belonging to @p slab back to the PMM.
static void slab_destroy(struct slab_cache *cache, struct slab *slab)
{
    size_t pages = cache->slab_size / PAGE_SIZE;
    uint64_t paddr = VIRT_TO_PHYS_ADDR((uintptr_t)slab);
    pmm_free_pages(paddr, pages);

    cache->total_slabs--;
    cache->total_objects -= cache->objs_per_slab;
}

// ── Public API - initialization ────────────────────────────────────────

void slab_init(void)
{
    kprintf("[SLAB] Initializing slab allocator...\n");

    all_caches = NULL;

    slab_cache_16   = slab_cache_create("size-16",     16, SLAB_NONE);
    slab_cache_32   = slab_cache_create("size-32",     32, SLAB_NONE);
    slab_cache_64   = slab_cache_create("size-64",     64, SLAB_NONE);
    slab_cache_128  = slab_cache_create("size-128",   128, SLAB_NONE);
    slab_cache_256  = slab_cache_create("size-256",   256, SLAB_NONE);
    slab_cache_512  = slab_cache_create("size-512",   512, SLAB_NONE);
    slab_cache_1024 = slab_cache_create("size-1024", 1024, SLAB_NONE);
    slab_cache_2048 = slab_cache_create("size-2048", 2048, SLAB_NONE);
    slab_cache_4096 = slab_cache_create("size-4096", 4096, SLAB_NONE);

    kprintf("[SLAB] Initialized 9 size-class caches\n");
}

// ── Public API - cache lifecycle ───────────────────────────────────────

struct slab_cache *slab_cache_create(const char *name, size_t obj_size,
                                     uint32_t flags)
{
    struct slab_cache *cache = kzalloc(sizeof(struct slab_cache));
    if (!cache)
        return NULL;

    strlcpy(cache->name, name, SLAB_NAME_MAX);
    cache->requested_size = obj_size;
    cache->obj_size       = obj_size;
    cache->flags          = flags;
    cache->lock           = (spinlock_t)SPINLOCK_INIT;

    // Zero all per-core magazines (kzalloc already zeroed, but be explicit)
    for (int i = 0; i < SMP_MAX_CORES; ++i)
        cache->magazines[i].count = 0;

    cache_calculate(cache);

    cache->next_cache = all_caches;
    all_caches = cache;

    kprintf("[SLAB] Created cache '%s': req=%lu, effective=%lu, per_slab=%lu\n",
            name, (uint64_t)obj_size, (uint64_t)cache->obj_size,
            (uint64_t)cache->objs_per_slab);

    return cache;
}

void slab_cache_destroy(struct slab_cache *cache)
{
    if (!cache)
        return;

    uint64_t flags;
    spin_lock_irqsave(&cache->lock, &flags);

    // Drain all per-core magazines back to the shared cache
    for (int c = 0; c < SMP_MAX_CORES; ++c)
    {
        struct slab_magazine *mag = &cache->magazines[c];
        while (mag->count > 0)
            slab_free_locked(cache, mag->objects[--mag->count]);
    }

    if (cache->active_objects > 0)
        kprintf("[SLAB] WARNING: Destroying cache '%s' with %lu active objects\n",
                cache->name, (uint64_t)cache->active_objects);

    struct slab *s, *next;

    for (s = cache->slabs_partial; s; s = next)
    {
        next = s->next;
        slab_destroy(cache, s);
    }
    for (s = cache->slabs_full; s; s = next)
    {
        next = s->next;
        slab_destroy(cache, s);
    }
    for (s = cache->slabs_empty; s; s = next)
    {
        next = s->next;
        slab_destroy(cache, s);
    }

    spin_unlock_irqrestore(&cache->lock, flags);

    // Remove from the global cache list
    struct slab_cache **pp = &all_caches;
    while (*pp && *pp != cache)
        pp = &(*pp)->next_cache;
    if (*pp)
        *pp = cache->next_cache;

    kfree(cache);
}

// ── Public API - allocation ────────────────────────────────────────────

/**
 * @brief Allocate from the shared cache (caller must hold @c cache->lock).
 *
 * @return Object pointer, or @c NULL on failure.
 */
static void *slab_alloc_locked(struct slab_cache *cache)
{
    struct slab *slab = NULL;

    // 1. Try a partial slab.
    if (cache->slabs_partial)
        slab = cache->slabs_partial;
    // 2. Promote an empty slab to partial
    else if (cache->slabs_empty)
    {
        slab = cache->slabs_empty;
        slab_list_remove(&cache->slabs_empty, slab);
        slab_list_add(&cache->slabs_partial, slab);
    }
    // 3. Allocate a brand-new slab
    else
    {
        slab = slab_create(cache);
        if (!slab)
        {
            if (cache->flags & SLAB_PANIC)
            {
                kprintf("[SLAB] PANIC: OOM in cache '%s'\n", cache->name);
                for (;;)
                    __asm__ volatile("cli; hlt");
            }
            return NULL;
        }
        slab_list_add(&cache->slabs_partial, slab);
    }

    // Pop one object from the slab's free list
    void *obj = slab->free_list;
    slab->free_list = *(void **)obj;
    slab->in_use++;
    cache->active_objects++;

    // If the slab is now full, move it to the full list
    if (slab->in_use == slab->total)
    {
        slab_list_remove(&cache->slabs_partial, slab);
        slab_list_add(&cache->slabs_full, slab);
        slab->was_full = true;
    }

    return obj;
}

void *slab_alloc(struct slab_cache *cache)
{
    if (!cache)
        return NULL;

    // Fast path: pop from the local core's magazine (no lock)
    int core = smp_core_id();
    struct slab_magazine *mag = &cache->magazines[core];

    if (mag->count > 0)
    {
        void *obj = mag->objects[--mag->count];
        if (cache->flags & SLAB_ZERO)
            memset(obj, 0, cache->obj_size);
        return obj;
    }

    // Slow path: the magazine is empty - take the cache lock and
    // refill the magazine with a batch of objects
    uint64_t flags;
    spin_lock_irqsave(&cache->lock, &flags);

    int refill = SLAB_MAGAZINE_SIZE / 2;
    if (refill < 1)
        refill = 1;

    for (int i = 0; i < refill; ++i)
    {
        void *obj = slab_alloc_locked(cache);
        if (!obj)
            break;
        mag->objects[mag->count++] = obj;
    }

    spin_unlock_irqrestore(&cache->lock, flags);

    // Pop one from the freshly refilled magazine
    if (mag->count > 0)
    {
        void *obj = mag->objects[--mag->count];
        if (cache->flags & SLAB_ZERO)
            memset(obj, 0, cache->obj_size);
        return obj;
    }

    return NULL;  // OOM
}

// ── Public API - deallocation ──────────────────────────────────────────

/// Return an object to the shared cache (cache holds @c cache->lock).
static void slab_free_locked(struct slab_cache *cache, void *obj)
{
    uintptr_t obj_addr  = (uintptr_t)obj;
    uintptr_t slab_base = obj_addr & ~(cache->slab_size - 1);
    struct slab *slab  = (struct slab *)slab_base;

    // Validate ownership
    if (slab->cache != cache)
    {
        kprintf("[SLAB] ERROR: Free to wrong cache! obj=%p, expected='%s'\n",
                obj, cache->name);
        return;
    }

    // Push the object back onto the slab's free list
    *(void **)obj   = slab->free_list;
    slab->free_list = obj;
    slab->in_use--;
    cache->active_objects--;

    // Move the slab between lists based on its new state
    if (slab->in_use == 0)
    {
        // Slab is now completely empty
        if (slab->was_full)
            slab_list_remove(&cache->slabs_full, slab);
        else
            slab_list_remove(&cache->slabs_partial, slab);
        slab->was_full = false;
        slab_list_add(&cache->slabs_empty, slab);
    }
    else if (slab->was_full)
    {
        // Was full, now partial
        slab_list_remove(&cache->slabs_full, slab);
        slab_list_add(&cache->slabs_partial, slab);
        slab->was_full = false;
    }
    // Was partial, still partial - nothing to do
}

void slab_free(struct slab_cache *cache, void *obj)
{
    if (!cache || !obj)
        return;

    // Fast path: push onto the local core's magazine (no lock)
    int core = smp_core_id();
    struct slab_magazine *mag = &cache->magazines[core];

    if (mag->count < SLAB_MAGAZINE_SIZE)
    {
        mag->objects[mag->count++] = obj;
        return;
    }

    // Slow path: magazine is full - drain half of it back to the shared
    // cache under the lock, then push the object
    uint64_t flags;
    spin_lock_irqsave(&cache->lock, &flags);

    int drain = SLAB_MAGAZINE_SIZE / 2;
    for (int i = 0; i < drain; ++i)
    {
        void *drained = mag->objects[--mag->count];
        slab_free_locked(cache, drained);
    }

    spin_unlock_irqrestore(&cache->lock, flags);

    // Now there's room in the magazine
    mag->objects[mag->count++] = obj;
}

// ── Public API - magazine flushing ─────────────────────────────────────

void slab_cache_drain_magazines(struct slab_cache *cache)
{
    uint64_t flags;
    spin_lock_irqsave(&cache->lock, &flags);
    for (int c = 0; c < SMP_MAX_CORES; ++c)
    {
        struct slab_magazine *mag = &cache->magazines[c];
        while (mag->count > 0)
            slab_free_locked(cache, mag->objects[--mag->count]);
    }
    spin_unlock_irqrestore(&cache->lock, flags);
}

// ── Public API - reclamation ───────────────────────────────────────────

size_t slab_cache_shrink(struct slab_cache *cache)
{
    if (!cache)
        return 0;

    size_t freed = 0;
    struct slab *s, *next;

    for (s = cache->slabs_empty; s; s = next)
    {
        next = s->next;
        freed += cache->slab_size / PAGE_SIZE;
        slab_list_remove(&cache->slabs_empty, s);
        slab_destroy(cache, s);
    }

    if (freed > 0)
        kprintf("[SLAB] Shrunk '%s': freed %lu pages\n",
                cache->name, (uint64_t)freed);

    return freed;
}

// ── Public API - generic size-class interface ──────────────────────────

/**
 * @brief Map a byte size to the appropriate size-class cache.
 * @return @c NULL for sizes larger than 4096 (caller should use
 *         @c kmalloc).
 */
static struct slab_cache *size_to_cache(size_t size)
{
    if (size <= 16)   return slab_cache_16;
    if (size <= 32)   return slab_cache_32;
    if (size <= 64)   return slab_cache_64;
    if (size <= 128)  return slab_cache_128;
    if (size <= 256)  return slab_cache_256;
    if (size <= 512)  return slab_cache_512;
    if (size <= 1024) return slab_cache_1024;
    if (size <= 2048) return slab_cache_2048;
    if (size <= 4096) return slab_cache_4096;
    return NULL;
}

void *slab_alloc_size(size_t size)
{
    if (size == 0)
        return NULL;

    struct slab_cache *cache = size_to_cache(size);
    if (cache)
        return slab_alloc(cache);

    // Too large for slab - fall back to heap
    return kmalloc(size);
}

void slab_free_size(void *ptr, size_t size)
{
    if (!ptr)
        return;

    struct slab_cache *cache = size_to_cache(size);
    if (cache)
    {
        slab_free(cache, ptr);
        return;
    }

    // Was allocated via kmalloc fallback
    kfree(ptr);
}

// ── Diagnostics ────────────────────────────────────────────────────────────

void slab_cache_get_stats(struct slab_cache *cache, struct slab_stats *stats)
{
    if (!cache || !stats)
        return;

    stats->name               = cache->name;
    stats->obj_size           = cache->requested_size;
    stats->effective_obj_size = cache->obj_size;
    stats->obj_count          = cache->total_objects;
    stats->active_count       = cache->active_objects;
    stats->slab_count         = cache->total_slabs;
    stats->pages_used         = cache->total_slabs
                                * (cache->slab_size / PAGE_SIZE);
}

void slab_debug_dump(void)
{
    kprintf("[SLAB] === Cache Summary ===\n");
    kprintf("  %s %s %s %s %s %s\n",
            "Name", "ReqSz", "EffSz", "Total", "Active", "Slabs");

    struct slab_cache *cache = all_caches;
    while (cache)
    {
        kprintf("  %s\t%lu\t%lu\t%lu\t%lu\t%lu\n",
                cache->name,
                (uint64_t)cache->requested_size,
                (uint64_t)cache->obj_size,
                (uint64_t)cache->total_objects,
                (uint64_t)cache->active_objects,
                (uint64_t)cache->total_slabs);
        cache = cache->next_cache;
    }
}

