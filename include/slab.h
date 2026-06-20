/**
 * @file slab.h
 * @brief Slab allocator interface.
 *
 * Provides fast, fragmentation-free allocation of fixed-size objects.
 * Each "slab cache" manages objects of a single size. Objects are
 * allocated from "slabs" (one or more contiguous pages) and returned
 * to a per-slab free list on deallocation.
 *
 * @par Design
 * - Named caches for specific kernel object types (e.g., network
 *   buffers, descriptors, page tables).
 * - Generic size-class caches (16..4096 bytes) created at init,
 *   used by @c slab_alloc_size() for callers that just need N bytes.
 * - Free objects store a next-pointer in-place (no per-object
 *   overhead while free).
 * - Slabs track their parent cache, enabling kfree-style freeing
 *   without requiring the caller to pass the size.
 *
 * @par Alignment
 * All slab objects are aligned to @c SLAB_MIN_ALIGN (16 bytes) or the
 * object size, whichever is smaller (but never less than
 * @c sizeof(void*)). Custom alignment can be specified at cache
 * creation time.
 *
 * @par Memory hierarchy (bottom to top)
 * @code
 *   PMM (pages) → Slab (fixed-size objects) → Caller
 * @endcode
 *
 * Depends on the PMM for page allocation and the heap for cache
 * metadata. Must be initialized after @c heap_init().
 */

#ifndef EMMK_SLAB_H
#define EMMK_SLAB_H

#include "types.h"

/// @name Configuration
/// @{

/// Minimum alignment for all slab-allocated objects.
#define SLAB_MIN_ALIGN  16

/// Maximum cache name length (including null terminator).
#define SLAB_NAME_MAX   32
/// @}

/// @name Cache flags
/// @{
#define SLAB_NONE     0x00  ///< No special behavior.
#define SLAB_ZERO     0x01  ///< Zero object memory on allocation.
#define SLAB_PANIC    0x02  ///< Panic (halt) on allocation failure.
#define SLAB_RECLAIM  0x04  ///< Cache is eligible for shrinking under OOM.
/// @}

/// @name Opaque types
/// @{

/**
 * @brief Opaque handle to a slab cache.
 *
 * Created with @c slab_cache_create(); holds all metadata for a single
 * object-size class. The internal structure is defined in slab.c.
 */
struct slab_cache;
/// @}

/// @name Statistics
/// @{

/// Snapshot of a single cache's state for diagnostics.
struct slab_stats
{
    const char *name;           ///< Cache name (pointer into cache struct).
    size_t obj_size;            ///< Object size (as specified at creation).
    size_t effective_obj_size;  ///< Actual per-object footprint (aligned).
    size_t obj_count;           ///< Total object slots across all slabs.
    size_t active_count;        ///< Currently allocated (in-use) objects.
    size_t slab_count;          ///< Number of slabs in this cache.
    size_t pages_used;          ///< Total pages consumed by all slabs.
};
/// @}

/// @name Initialization
/// @{

/**
 * @brief Initialize the slab allocator subsystem.
 *
 * Creates the generic size-class caches (16, 32, 64, ... 4096 bytes).
 *
 * @note Must be called after @c heap_init().
 */
void slab_init(void);
/// @}

/// @name Cache lifecycle
/// @{

/**
 * @brief Create a new slab cache for a specific object size.
 *
 * @param name      Human-readable name (truncated to @c SLAB_NAME_MAX).
 * @param obj_size  Size of each object in bytes (rounded up to
 *                  @c SLAB_MIN_ALIGN if too small).
 * @param flags     Bitwise OR of @c SLAB_* flags.
 *
 * The cache starts empty; the first slab is allocated on the first
 * @c slab_alloc() call.
 *
 * @return Pointer to the new cache, or @c NULL on failure.
 */
struct slab_cache *slab_cache_create(const char *name, size_t obj_size,
                                     uint32_t flags);

/**
 * @brief Destroy @p cache and free all of its slabs.
 *
 * All objects in the cache must have been freed before calling this.
 * If active objects remain, a warning is logged and the cache is
 * destroyed anyway (leaking those objects).
 */
void slab_cache_destroy(struct slab_cache *cache);
/// @}

/// @name Object allocation / deallocation
/// @{

/**
 * @brief Allocate one object from @p cache.
 *
 * If no free objects are available, a new slab (one or more pages) is
 * allocated from the PMM. If @c SLAB_ZERO is set on the cache, the
 * returned object is zeroed.
 *
 * @return Pointer to the object, or NULL on failure (unless
 *         @c SLAB_PANIC is set, in which case the kernel halts).
 */
void *slab_alloc(struct slab_cache *cache);

/**
 * @brief Return object @p obj to @p cache.
 *
 * The object is placed back on the slab's free list. Passing an
 * object that did not originate from @p cache results in undefined
 * behavior (corruption of the free list).
 */
void slab_free(struct slab_cache *cache, void *obj);
/// @}

/// Flush magazines back to the shared cache.
void slab_cache_drain_magazines(struct slab_cache *cache);

/// @name Memory reclamation
/// @{

/**
 * @brief Release completely empty slabs back to the PMM.
 *
 * Walks the slab list and frees any slab with zero active objects.
 *
 * @param cache  Cache to shrink.
 * @return Number of pages freed.
 */
size_t slab_cache_shrink(struct slab_cache *cache);
/// @}

/// @name Diagnostics
/// @{

/// Populate @p stats for a single @p cache.
void slab_cache_get_stats(struct slab_cache *cache, struct slab_stats *stats);

/// Print a summary of all slab caches to serial.
void slab_debug_dump(void);
/// @}

/// @name Generic size-class caches
/// @{
extern struct slab_cache *slab_cache_16;
extern struct slab_cache *slab_cache_32;
extern struct slab_cache *slab_cache_64;
extern struct slab_cache *slab_cache_128;
extern struct slab_cache *slab_cache_256;
extern struct slab_cache *slab_cache_512;
extern struct slab_cache *slab_cache_1024;
extern struct slab_cache *slab_cache_2048;
extern struct slab_cache *slab_cache_4096;

/**
 * @brief Allocate from the appropriate size-class cache.
 *
 * @param size  Bytes needed (rounded up to the next power-of-two cache).
 * @return Pointer to the memory, or NULL on failure.
 *
 * @note For sizes > 4096, falls back to @c kmalloc().
 */
void *slab_alloc_size(size_t size);

/**
 * @brief Free memory allocated with @c slab_alloc_size().
 *
 * @param ptr   Pointer to free.
 * @param size  Original size passed to @c slab_alloc_size().
 *
 * @warning @p size must exactly match the value passed to
 *          @c slab_alloc_size(). Passing the wrong size routes the
 *          object to the wrong cache, silently corrupting it.
 */
void slab_free_size(void *ptr, size_t size);
/// @}

#endif // EMMK_SLAB_H

