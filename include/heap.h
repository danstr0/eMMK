/**
 * @file heap.h
 * @brief General-purpose kernel heap allocator.
 *
 * Provides @c kmalloc / <code>kfree</code>-style dynamic memory allocation
 * for kernel use. Internally uses a linked-list free list with first-fit 
 * allocation and block coalescing on free.
 *
 * @par Alignment guarantee
 * All allocations are aligned to at least @c HEAP_ALIGNMENT bytes (16),
 * satisfying x86-64 ABI requirements and SSE operand alignment.
 *
 * @par Backing memory
 * The heap obtains physical pages from the PMM and maps them via the VMM.
 * It starts at @c HEAP_INITIAL_SIZE and grows on demand up to
 * @c HEAP_MAX_SIZE.
 */

#ifndef EMMK_HEAP_H
#define EMMK_HEAP_H

#include "types.h"

/// @name Configuration
/// @{

/// Initial heap size allocated during @c heap_init().
#define HEAP_INITIAL_SIZE  (64 * 1024)

/// Maximum heap size (bytes).
#define HEAP_MAX_SIZE      (16 * 1024 * 1024)

/// Minimum alignment for all heap allocations (bytes).
#define HEAP_ALIGNMENT      16

/// Minimum user-visible allocation size — smaller requests are rounded up.
#define HEAP_MIN_ALLOC      32
/// @}

/// @name Statistics
/// @{

/// Snapshot of heap state for diagnostics.
struct heap_stats
{
    size_t total_size;       ///< Total heap size (bytes).
    size_t used_size;        ///< Bytes currently allocated.
    size_t free_size;        ///< Bytes currently free.
    size_t num_allocs;       ///< Lifetime allocation count.
    size_t num_frees;        ///< Lifetime free count.
    size_t num_free_blocks;  ///< Current number of free-list entries.
};
/// @}

/// @name Initialization
/// @{

/**
 * @brief Initialize the kernel heap.
 *
 * Allocates @c HEAP_INITIAL_SIZE bytes of physical memory from the PMM,
 * maps them into the kernel virtual address space, and sets up the free
 * list with a single large block. Must be called after @c pmm_init()
 * and @c vmm_init().
 */
void heap_init(void);
/// @}

/// @name Allocation
/// @{

/**
 * @brief Allocate @p size bytes of kernel memory.
 *
 * @param size  Number of bytes to allocate.
 * @return A pointer to at least @p size bytes of uninitialized memory,
 *         aligned to @c HEAP_ALIGNMENT. Returns @c NULL if the 
 *         allocation cannot be satisfied.
 */
void *kmalloc(size_t size);

/**
 * @brief Allocate @p size bytes of zeroed kernel memory.
 *
 * Equivalent to @c kmalloc() followed by @c memset(ptr,0,size).
 * 
 * @return @c NULL on failure.
 */
void *kzalloc(size_t size);

/**
 * @brief Resize an existing allocation.
 * 
 * @param ptr       Pointer returned by a previous allocation, or @c NULL.
 * @param new_size  Desired new size in bytes.
 *
 * Behavior:
 * - If @p ptr is NULL, equivalent to @c kmalloc(new_size).
 * - If @p new_size is 0 and @p ptr is non-NULL, frees @p ptr and
 *   returns @c NULL.
 * - On success, returns a pointer to the resized memory (may differ
 *   from @p ptr). Contents up to @c min(old_size,new_size) are
 *   preserved.
 * - On failure, returns @c NULL and the original allocation at @p ptr
 * remains valid and unchanged.
 */
void *krealloc(void *ptr, size_t new_size);

/**
 * @brief Allocate @p size bytes of memory with a specific alignment.
 *
 * @param size       Number of bytes to allocate.
 * @param alignment  Required alignment (must be a power of two and
 *                   ≥ @c HEAP_ALIGNMENT).
 * @return An aligned pointer, or @c NULL on failure.
 *
 * @note Pointers returned by this function must be freed with @c kfree()
 *       like any other allocation. The allocator stores alignment
 *       metadata that @c kfree() uses to locate the underlying block.
 */
void *kaligned_alloc(size_t size, size_t alignment);
/// @}

/// @name Deallocation
/// @{

/**
 * @brief Free kernel memory.
 *
 * @param ptr  Pointer returned by @c kmalloc / @c kzalloc / @c krealloc /
 *             @c kaligned_alloc, or @c NULL (safe no-op).
 *
 * The freed block is returned to the free list and coalesced with
 * adjacent free blocks if possible.
 */
void kfree(void *ptr);
/// @}

/// @name Diagnostics
/// @{

/**
 * @brief Populate @p stats with a snapshot of the current heap state.
 * @param stats  Caller-allocated output structure.
 */
void heap_get_stats(struct heap_stats *stats);

/// Print a detailed dump of the heap free list to serial.
void heap_dump(void);
/// @}

/// @name Convenience macros
/// @{

/// Allocate memory for a single instance of @p type.
#define kmalloc_type(type)  ((type *)kmalloc(sizeof(type)))

/// Allocate zeroed memory for a single instance of @p type.
#define kzalloc_type(type)  ((type *)kzalloc(sizeof(type)))

/**
 * @brief Allocate an array with integer-overflow checking.
 * @return @c NULL if @p count * @p size would overflow @c size_t.
 */
static inline void *kmalloc_array(size_t count, size_t size)
{
    if (count != 0 && size > (size_t)-1 / count)
        return NULL;
    return kmalloc(count * size);
}

/**
 * @brief Allocate a zeroed array with integer-overflow checking.
 * @return @c NULL if @p count * @p size would overflow @c size_t.
 */
static inline void *kzalloc_array(size_t count, size_t size)
{
    if (count != 0 && size > (size_t)-1 / count)
        return NULL;
    return kzalloc(count * size);
}
/// @}

#endif // EMMK_HEAP_H

