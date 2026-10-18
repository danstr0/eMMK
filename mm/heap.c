/**
 * @file heap.c
 * @brief Kernel heap allocator (first-fit, coalescing free list).
 *
 * @par Memory layout
 * @code{.txt}
 *   [block_header][user_data...][block_header][user_data...]...
 * @endcode
 *
 * Every block (free or allocated) has a @c block_header prepended. Free
 * blocks are additionally linked into a doubly-linked free list for fast
 * traversal.
 *
 * On free, adjacent free blocks are coalesced to reduce fragmentation.
 * On expansion, the new region is coalesced with the previous tail block
 * if it was free, preventing boundary fragmentation.
 *
 * The heap resides at a dedicated virtual address range
 * (@c HEAP_VIRTUAL_BASE) and grows upward by allocating physical pages
 * from the PMM one at a time and mapping them via the VMM. Heap memory is
 * therefore virtually contiguous but @b not physically contiguous, and
 * must never be used as a DMA buffer.
 */

#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "serial.h"
#include "string.h"
#include "spinlock.h"

// ── Block header ───────────────────────────────────────────────────────

/**
 * Stored immediately before every allocation (free or used).
 *
 * The doubly-linked physical-order list gives O(1) neighbor access for
 * coalescing; the doubly-linked free list gives O(1) removal.
 *
 * Aligned to @c HEAP_ALIGNMENT so its size is a multiple of it: heap
 * regions start page-aligned and block sizes are multiples of
 * @c HEAP_ALIGNMENT, so every user pointer (header + size) is aligned.
 */
struct block_header
{
    size_t size;              ///< User-visible size (excludes header).
    bool   is_free;
    struct block_header *next;       ///< Next block in physical order.
    struct block_header *prev;       ///< Previous block in physical order.
    struct block_header *next_free;  ///< Next block in free list (if free).
    struct block_header *prev_free;  ///< Previous block in free list.
    uint32_t magic;                  ///< @c BLOCK_MAGIC for corruption detection.
} ALIGNED(HEAP_ALIGNMENT);

STATIC_ASSERT(sizeof(struct block_header) % HEAP_ALIGNMENT == 0,
              "block header size must preserve HEAP_ALIGNMENT");

#define BLOCK_MAGIC        0xDEADBEEF
#define BLOCK_HEADER_SIZE  sizeof(struct block_header)

/**
 * Magic value stored just before an aligned pointer returned by
 * @c kaligned_alloc(). Allows @c kfree() to reliably distinguish
 * aligned allocations from normal ones.
 */
#define ALIGNED_MAGIC  0xA110CA7E

/**
 * Metadata prepended to the user-visible aligned pointer. Stored in
 * the padding between the underlying @c kmalloc block and the aligned
 * address.
 */
struct aligned_header
{
    uint32_t magic;     ///< @c ALIGNED_MAGIC.
    uint32_t pad;       ///< Padding for 8-byte alignment of this struct.
    void     *raw_ptr;  ///< Original pointer from @c kmalloc().
};

/// Alignment applied to all user allocation sizes.
#define ALIGN_UP_HEAP(x)   ALIGN_UP((x), HEAP_ALIGNMENT)

// ── Heap state ─────────────────────────────────────────────────────────

static struct block_header *heap_start;  ///< First block (physical order).
static struct block_header *heap_end;    ///< Last block (physical order).
static struct block_header *free_list;   ///< Head of free list.

static size_t   heap_total_size;
static size_t   heap_used_size;
static uint64_t heap_num_allocs;
static uint64_t heap_num_frees;

/// Base virtual address of the heap (@c HEAP_VIRTUAL_BASE).
static uint64_t heap_vaddr;

/**
 * Protects all heap state (block lists, free list, counters, vaddr).
 *
 * Contention is moderate: the heap backs slab metadata and occasional large
 * allocations. Hot-path object allocation goes through per-core slab caches,
 * which are lock-free.
 */
static spinlock_t heap_lock = SPINLOCK_INIT;

// ── Free list helpers ──────────────────────────────────────────────────

/// Remove @p block from the free list.
static void free_list_remove(struct block_header *block)
{
    if (block->prev_free)
        block->prev_free->next_free = block->next_free;
    else
        free_list = block->next_free;

    if (block->next_free)
        block->next_free->prev_free = block->prev_free;

    block->next_free = NULL;
    block->prev_free = NULL;
}

/// Insert @p block at the front of the free list.
static void free_list_add(struct block_header *block)
{
    block->next_free = free_list;
    block->prev_free = NULL;
    if (free_list)
        free_list->prev_free = block;
    free_list = block;
}

// ── Block validation ───────────────────────────────────────────────────

/// Check a block's magic number; returns @c false (and logs) on corruption.
static inline bool block_validate(struct block_header *block,
                                  const char *context)
{
    if (UNLIKELY(block->magic != BLOCK_MAGIC))
    {
        kprintf("[HEAP] CORRUPTION: Invalid magic at %p (%s)\n",
                (void *)block, context);
        return false;
    }
    return true;
}

// ── Heap expansion ─────────────────────────────────────────────────────

/**
 * @brief Back [@p vaddr, @p vaddr + @p num_pages pages) with fresh frames.
 *
 * Frames are allocated individually: the heap never needs physical
 * contiguity, so expansion cannot fail merely because RAM is fragmented.
 * On failure everything mapped so far is released.
 */
static bool heap_map_new_pages(uint64_t vaddr, size_t num_pages)
{
    for (size_t i = 0; i < num_pages; ++i)
    {
        uint64_t paddr = pmm_alloc_page();
        if (paddr == PMM_ALLOC_FAILED
            || vmm_map_page(vaddr + i * PAGE_SIZE, paddr,
                            PTE_KERNEL_RW) != VMM_OK)
        {
            if (paddr != PMM_ALLOC_FAILED)
                pmm_free_page(paddr);
            for (size_t j = 0; j < i; ++j)
                pmm_free_page(vmm_unmap_page(vaddr + j * PAGE_SIZE));
            return false;
        }
    }
    return true;
}

/**
 * @brief Grow the heap by at least @p min_size bytes.
 *
 * Allocates physical pages, maps them at the end of the heap, and
 * creates a new free block. If the current tail block (@c heap_end) is
 * free, the new region is coalesced with it to avoid boundary
 * fragmentation.
 */
static bool heap_expand(size_t min_size)
{
    size_t expand_size = ALIGN_UP_HEAP(min_size + BLOCK_HEADER_SIZE);
    expand_size = PAGE_ALIGN_UP(expand_size);

    if (heap_total_size + expand_size > HEAP_MAX_SIZE)
    {
        kprintf("[HEAP] Cannot expand: would exceed max (%lu KiB)\n",
                (uint64_t)(HEAP_MAX_SIZE / 1024));
        return false;
    }

    size_t num_pages = expand_size / PAGE_SIZE;
    uint64_t vaddr = heap_vaddr + heap_total_size;
    if (!heap_map_new_pages(vaddr, num_pages))
    {
        kprintf("[HEAP] Cannot expand: out of memory\n");
        return false;
    }

    heap_total_size += expand_size;

    // Coalesce with the tail block if it is free
    if (heap_end && heap_end->is_free)
    {
        free_list_remove(heap_end);
        heap_end->size += expand_size;
        free_list_add(heap_end);

        kprintf("[HEAP] Expanded by %lu KiB (coalesced), total %lu KiB\n",
                (uint64_t)(expand_size / 1024),
                (uint64_t)(heap_total_size / 1024));
        return true;
    }

    // Otherwise create a fresh free block
    struct block_header *new_block = (struct block_header *)vaddr;
    new_block->size      = expand_size - BLOCK_HEADER_SIZE;
    new_block->is_free   = true;
    new_block->magic     = BLOCK_MAGIC;
    new_block->next      = NULL;
    new_block->prev      = heap_end;
    new_block->next_free = NULL;
    new_block->prev_free = NULL;

    if (heap_end)
        heap_end->next = new_block;
    heap_end = new_block;

    free_list_add(new_block);

    kprintf("[HEAP] Expanded by %lu KiB, total %lu KiB\n",
            (uint64_t)(expand_size / 1024),
            (uint64_t)(heap_total_size / 1024));
    return true;
}

// ── Split helper ───────────────────────────────────────────────────────

/**
 * @brief Split @p block if it is large enough to hold @p size plus a
 *        new free block.
 *
 * If the remainder is too small to be useful, @p block is used as-is
 * to avoid creating unusably tiny blocks.
 */
static void block_try_split(struct block_header *block, size_t size)
{
    size_t remainder = block->size - size;
    if (remainder < BLOCK_HEADER_SIZE + HEAP_MIN_ALLOC)
        return;

    struct block_header *new_block = (struct block_header *)
        ((uint8_t *)block + BLOCK_HEADER_SIZE + size);

    new_block->size      = remainder - BLOCK_HEADER_SIZE;
    new_block->is_free   = true;
    new_block->magic     = BLOCK_MAGIC;
    new_block->next      = block->next;
    new_block->prev      = block;
    new_block->next_free = NULL;
    new_block->prev_free = NULL;

    if (block->next)
        block->next->prev = new_block;
    block->next = new_block;

    if (heap_end == block)
        heap_end = new_block;

    block->size = size;
    free_list_add(new_block);
}

// ── Coalescing helper ──────────────────────────────────────────────────

/**
 * @brief Merge @p block with adjacent free blocks.
 *
 * Attempts to merge with the next block first, then the previous block.
 * Returns the resulting (possibly merged) block, which is @b not yet on the
 * free list — the caller must add it.
 */
static struct block_header *block_coalesce(struct block_header *block)
{
    // Absorb the next block if free
    if (block->next && block->next->is_free)
    {
        struct block_header *next = block->next;
        free_list_remove(next);

        block->size += BLOCK_HEADER_SIZE + next->size;
        block->next = next->next;
        if (next->next)
            next->next->prev = block;
        if (heap_end == next)
            heap_end = block;

        next->magic = 0;  // invalidate absorbed header
    }

    // Merge into the previous block if free
    if (block->prev && block->prev->is_free)
    {
        struct block_header *prev = block->prev;
        free_list_remove(prev);

        prev->size += BLOCK_HEADER_SIZE + block->size;
        prev->next = block->next;
        if (block->next)
            block->next->prev = prev;
        if (heap_end == block)
            heap_end = prev;

        block->magic = 0;
        block = prev;
    }

    return block;
}

// ── Public API - initialization ────────────────────────────────────────

void heap_init(void)
{
    kprintf("[HEAP] Initializing kernel heap...\n");

    heap_vaddr      = HEAP_VIRTUAL_BASE;
    heap_total_size = 0;
    heap_used_size  = 0;
    heap_num_allocs = 0;
    heap_num_frees  = 0;
    heap_start      = NULL;
    heap_end        = NULL;
    free_list       = NULL;

    if (!heap_map_new_pages(heap_vaddr, HEAP_INITIAL_SIZE / PAGE_SIZE))
    {
        kprintf("[HEAP] ERROR: Cannot allocate initial pages\n");
        return;
    }

    // Single free block spanning the entire initial region
    heap_start = (struct block_header *)heap_vaddr;
    heap_start->size      = HEAP_INITIAL_SIZE - BLOCK_HEADER_SIZE;
    heap_start->is_free   = true;
    heap_start->magic     = BLOCK_MAGIC;
    heap_start->next      = NULL;
    heap_start->prev      = NULL;
    heap_start->next_free = NULL;
    heap_start->prev_free = NULL;

    heap_end        = heap_start;
    free_list       = heap_start;
    heap_total_size = HEAP_INITIAL_SIZE;

    kprintf("[HEAP] Initialized at %p, size %lu KiB\n",
            (void *)heap_vaddr, (uint64_t)(heap_total_size / 1024));
}

// ── Public API - allocation ────────────────────────────────────────────

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    uint64_t flags;
    spin_lock_irqsave(&heap_lock, &flags);

    size = ALIGN_UP_HEAP(size);
    if (size < HEAP_MIN_ALLOC)
        size = HEAP_MIN_ALLOC;

    // First-fit search through the free list
    struct block_header *block = free_list;
    while (block)
    {
        if (!block_validate(block, "kmalloc search"))
        {
            spin_unlock_irqrestore(&heap_lock, flags);
            return NULL;
        }
        if (block->size >= size)
            break;
        block = block->next_free;
    }

    // No suitable block - try expanding the heap
    if (!block)
    {
        if (!heap_expand(size))
        {
            spin_unlock_irqrestore(&heap_lock, flags);
            return NULL;
        }

        block = free_list;
        while (block)
        {
            if (block->size >= size)
                break;
            block = block->next_free;
        }
        if (!block)
        {
            spin_unlock_irqrestore(&heap_lock, flags);
            return NULL;
        }
    }

    // Remove from free list, split if worthwhile, mark allocated
    free_list_remove(block);
    block_try_split(block, size);

    block->is_free = false;
    heap_used_size += block->size;
    heap_num_allocs++;

    spin_unlock_irqrestore(&heap_lock, flags);
    return (void *)((uint8_t *)block + BLOCK_HEADER_SIZE);
}

void *kzalloc(size_t size)
{
    void *ptr = kmalloc(size);
    if (ptr)
        memset(ptr, 0, size);
    return ptr;
}

void *kaligned_alloc(size_t size, size_t alignment)
{
    if (size == 0 || alignment == 0)
        return NULL;

    if (alignment <= HEAP_ALIGNMENT)
        return kmalloc(size);

    // Over-allocate so there is room for the aligned_header and enough
    // space to shift the user pointer forward to the next alignment
    // boundary
    size_t overhead = alignment + sizeof(struct aligned_header);
    void *raw = kmalloc(size + overhead);
    if (!raw)
        return NULL;

    uintptr_t raw_addr = (uintptr_t)raw;
    uintptr_t aligned = ALIGN_UP(raw_addr + sizeof(struct aligned_header),
                                 alignment);

    // Store metadata immediately before the aligned pointer
    struct aligned_header *hdr =
        (struct aligned_header *)(aligned - sizeof(struct aligned_header));
    hdr->magic   = ALIGNED_MAGIC;
    hdr->pad     = 0;
    hdr->raw_ptr = raw;

    return (void *)aligned;
}

// ── Public API - deallocation ──────────────────────────────────────────

void kfree(void *ptr)
{
    if (!ptr)
        return;

    uint64_t flags;
    spin_lock_irqsave(&heap_lock, &flags);

    // Check whether this is an aligned allocation by looking for
    // ALIGNED_MAGIC
    struct aligned_header *ahdr =
        (struct aligned_header *)((uint8_t *)ptr
                                  - sizeof(struct aligned_header));

    if (ahdr->magic == ALIGNED_MAGIC)
    {
        ahdr->magic = 0;      // prevent double-detection
        ptr = ahdr->raw_ptr;  // redirect to kmalloc ptr
    }

    struct block_header *block = (struct block_header *)
        ((uint8_t *)ptr - BLOCK_HEADER_SIZE);

    if (!block_validate(block, "kfree"))
    {
        spin_unlock_irqrestore(&heap_lock, flags);
        return;
    }

    if (block->is_free)
    {
        kprintf("[HEAP] WARNING: Double-free detected at %p\n", ptr);
        spin_unlock_irqrestore(&heap_lock, flags);
        return;
    }

    block->is_free = true;
    heap_used_size -= block->size;
    heap_num_frees++;

    // Coalesce with neighbors and return to the free list
    block = block_coalesce(block);
    free_list_add(block);

    spin_unlock_irqrestore(&heap_lock, flags);
}

void *krealloc(void *ptr, size_t new_size)
{
    if (!ptr)
        return kmalloc(new_size);

    if (new_size == 0)
    {
        kfree(ptr);
        return NULL;
    }

    uint64_t flags;
    spin_lock_irqsave(&heap_lock, &flags);

    struct block_header *block = (struct block_header *)
        ((uint8_t *)ptr - BLOCK_HEADER_SIZE);

    if (!block_validate(block, "krealloc"))
    {
        spin_unlock_irqrestore(&heap_lock, flags);
        return NULL;
    }

    new_size = ALIGN_UP_HEAP(new_size);
    if (new_size < HEAP_MIN_ALLOC)
        new_size = HEAP_MIN_ALLOC;

    // Current block is already large enough
    if (block->size >= new_size)
    {
        spin_unlock_irqrestore(&heap_lock, flags);
        return ptr;
    }

    // Try to absorb the next block if it is free and adjacent
    if (block->next && block->next->is_free)
    {
        size_t combined = block->size + BLOCK_HEADER_SIZE
                          + block->next->size;
        if (combined >= new_size)
        {
            size_t old_size = block->size;
            struct block_header *next = block->next;
            free_list_remove(next);

            block->size = combined;
            block->next = next->next;
            if (next->next)
                next->next->prev = block;
            if (heap_end == next)
                heap_end = block;
            next->magic = 0;

            block_try_split(block, new_size);

            heap_used_size += block->size - old_size;

            spin_unlock_irqrestore(&heap_lock, flags);
            return ptr;
        }
    }

    // Fall back to allocate-copy-free. Release the lock
    // before calling kmalloc/kfree since they take it.
    size_t copy_size = block->size;
    spin_unlock_irqrestore(&heap_lock, flags);

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr)
        return NULL;

    memcpy(new_ptr, ptr, copy_size);
    kfree(ptr);

    return new_ptr;
}

// ── Diagnostics ────────────────────────────────────────────────────────

void heap_get_stats(struct heap_stats *stats)
{
    if (!stats)
        return;

    stats->total_size = heap_total_size;
    stats->used_size  = heap_used_size;
    stats->free_size  = heap_total_size - heap_used_size;
    stats->num_allocs = heap_num_allocs;
    stats->num_frees  = heap_num_frees;

    stats->num_free_blocks = 0;
    struct block_header *block = free_list;
    while (block)
    {
        stats->num_free_blocks++;
        block = block->next_free;
    }
}

void heap_dump(void)
{
    kprintf("[HEAP] === Heap Dump ===\n");
    kprintf("[HEAP] Total: %lu KiB, Used: %lu KiB, Free: %lu KiB\n",
            (uint64_t)(heap_total_size / 1024),
            (uint64_t)(heap_used_size / 1024),
            (uint64_t)((heap_total_size - heap_used_size) / 1024));
    kprintf("[HEAP] Allocations: %lu, Frees: %lu\n",
            heap_num_allocs, heap_num_frees);

    kprintf("[HEAP] Block list (physical order):\n");
    struct block_header *block = heap_start;
    int count = 0;
    while (block && count < 32)
    {
        kprintf("       %p: %lu bytes, %s, magic=%s\n",
                (void *)block,
                (uint64_t)block->size,
                block->is_free ? "FREE" : "USED",
                (block->magic == BLOCK_MAGIC) ? "OK" : "BAD");
        block = block->next;
        count++;
    }
    if (block)
        kprintf("       ... (more blocks)\n");
}

