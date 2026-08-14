==============
Slab Allocator
==============

Overview
========

eMMK's Slab Allocator manages caches of uniform-sized objects. By
pre-allocating contiguous physical pages from the PMM and carving them
into fixed slots, it provides ``O(1)`` allocation and deallocation while
eliminating external fragmentation.

The system uses a two-tier allocation hierarchy:

1. **Per-Core Magazines (Fast Path):** Lockless, CPU-local arrays that
   service allocations without lock contention.
2. **Shared Slab Cache (Slow Path):** Spinlock-protected pool of full,
   partial, and empty slabs used to refill or drain per-core magazines.

.. code-block:: text

    ┌───────────────────────────────────────────────────────────┐
    │                     Callers (Per Core)                    │
    └─────────────────────────────┬─────────────────────────────┘
                                  │
                   [Fast Path]    │  (Lockless pop/push)
                                  v
                   ┌──────────────────────────────┐
                   │  struct slab_magazine        │
                   │  (SLAB_MAGAZINE_SIZE = 32)   │
                   └──────────────┬───────────────┘
                                  │
                   [Slow Path]    │  (Batch refill/drain under cache spinlock)
                                  v
    ┌───────────────────────────────────────────────────────────┐
    │                   struct slab_cache                       │
    │  ┌──────────────────┐ ┌─────────────────┐ ┌────────────┐  │
    │  |  slabs_partial   | |   slabs_full    | | slabs_empty|  │
    │  └────────┬─────────┘ └─────────────────┘ └─────┬──────┘  │
    └───────────│─────────────────────────────────────│─────────┘
                │                                     │
                v                                     v
       [Pages from PMM]                      [Reclaimed via shrink]


Design Principles
=================
- **Zero Per-Object Metadata Overhead:** Free slots form an in-place
  linked list where the first ``sizeof(void *)`` bytes store the pointer
  to the next free slot.
- **Power-of-Two Slab Alignments:** Slab sizes (1 to 8 pages) are strictly
  rounded to powers of two. This invariant allows ``slab_free_locked()``
  to compute the enclosing ``struct slab`` header using address masking:
  
  .. code-block:: c

      uintptr_t slab_base = obj_addr & ~(cache->slab_size - 1);
      struct slab *slab = (struct slab *)slab_base;

- **Per-CPU Concurrency Scaling:** Allocation and deallocation hit local
  magazines first. Spinlock contention on ``cache->lock`` only occurs
  during batch refills or drains.

Cache Types
===========

Dedicated Caches
----------------
Dedicated caches manage high-frequency kernel structures (e.g., process
descriptors, network buffers). They provide isolated memory pools to
prevent cross-subsystem starvation and enable fine-grained diagnostics.

.. code-block:: c

    struct slab_cache *proc_cache = slab_cache_create("proc_t", sizeof(struct process), SLAB_ZERO);
    
    // Fast lockless retrieval if local magazine is non-empty
    struct process *p = slab_alloc(proc_cache);
    
    slab_free(proc_cache, p);

Generic Size Classes
--------------------

During ``slab_init()``, nine generic power-of-two caches (16 B to 4096 B)
are created. The helper ``slab_alloc_size()`` routes requests to these
caches:

- Sizes **<= 4096 bytes** are serviced by the corresponding slab size class.
- Sizes **> 4096 bytes** fall back to ``kmalloc()``.

Configuration Flags
===================

.. list-table:: **Slab Configuration Flags**
   :header-rows: 1
   :widths: 25 75

   * - Flag
     - Description
   * - ``SLAB_NONE``
     - Default allocation behavior.
   * - ``SLAB_ZERO``
     - Zeroes memory via ``memset()`` before returning the object to
       the caller.
   * - ``SLAB_PANIC``
     - Causes the kernel to halt (CLI/HLT loop) on allocation failure
       rather than returning ``NULL``.
   * - ``SLAB_RECLAIM``
     - Identifies caches eligible for automated shrinking under memory
       pressure.

API Reference
=============

.. list-table:: **Core Subsystem Functions**
   :header-rows: 1
   :widths: 30 70

   * - Function
     - Description
   * - ``slab_init()``
     - Initializes generic size-class caches (16 B to 4096 B). Must run
       after ``heap_init()``.
   * - ``slab_cache_create()``
     - Allocates metadata and calculates alignment and slab parameters for
       a new cache.
   * - ``slab_cache_destroy()``
     - Drains all CPU magazines, frees all associated slabs to the PMM,
       and frees cache metadata.
   * - ``slab_alloc()``
     - Returns an object slot from the local magazine or refills from the
       shared cache.
   * - ``slab_free()``
     - Returns an object to the local magazine or drains a batch to the
       shared cache.
   * - ``slab_cache_drain_magazines()``
     - Flushes all CPU-local magazine objects back to the shared slab
       lists.
   * - ``slab_cache_shrink()``
     - Releases all empty slabs in ``slabs_empty`` back to the PMM.
   * - ``slab_debug_dump()``
     - Dumps active object counts, effective object sizes, and page usage
       to serial log.

Memory Reclamation & Synchronization
====================================

When system physical memory is constrained, ``slab_cache_shrink()``
reclaims unused page frames.

.. note::

   Because per-core magazines hold cached objects locally, call 
   ``slab_cache_drain_magazines(cache)`` prior to ``slab_cache_shrink(cache)``
   to ensure all inactive objects are returned to their parent slabs before
   checking for empty slabs.

Critical Safety Warnings
========================

1. **Exact-Size Requirement for Generic Allocations:**
   When using ``slab_free_size(ptr, size)``, the caller must pass the
   exact size passed to ``slab_alloc_size()``. 

   .. warning::

      Passing an incorrect size routes the pointer to the wrong size-class
      cache, corrupting that cache's free list or passing a slab-managed
      pointer to ``kfree()``.

2. **Cross-Core Freeing Mechanics:**
   Objects allocated on CPU A may be freed on CPU B. The object will enter
   CPU B's local magazine. If CPU B's magazine overflows, the drain step
   uses address-masking on the object pointer to resolve its parent slab
   header and return it to the central cache structure.

Diagnostics
===========

Diagnostics can be triggered via ``slab_debug_dump()`` to inspect memory
utilization across all registered caches:

.. code-block:: text

    [SLAB] === Cache Summary ===
      Name          ReqSz   EffSz   Total   Active  Slabs
      size-16       16      16      252     42      1
      size-64       64      64      120     102     2
      proc_t        512     512     16      14      2

Reference
=========

- J. Bonwick, "The slab allocator: An object-caching kernel memory
  allocator," in Proc. USENIX Summer 1994 Tech. Conf., Boston, MA, USA,
  1994, pp. 87–98.

Related Files
=============

- ``include/slab.h``
- ``mm/slab.c``

