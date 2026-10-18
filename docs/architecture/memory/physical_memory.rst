=======================
Physical Memory Manager
=======================

Overview
========
The Physical Memory Manager (PMM) is responsible for tracking and
allocating raw 4 KiB physical page frames. It serves as the foundation for
the Virtual Memory Manager (VMM) and the kernel heap.

The PMM uses a bitmap allocator to track state. This approach provides a
compact, deterministic representation of physical memory where every bit
corresponds to a unique 4 KiB frame.

.. list-table:: **Manager characteristics**
   :widths: 30 70

   * - **Allocation Granularity**
     - 4 KiB (Page Frame).
   * - **Metadata Structure**
     - Bitmap (1 bit per page).
   * - **Address Space**
     - Physical (RAM).
   * - **Overhead**
     - ~32 KiB of metadata per 1 GiB of RAM.
   * - **Search Strategy**
     - First-fit linear scan (for contiguous blocks).
   * - **Concurrency Model**
     - Spinlock-protected (``spin_lock_irqsave``).

The Bitmap Model
================
The PMM views physical memory as an array of bits. A bit value of 1
indicates the page is either currently allocated or reserved by the
hardware/firmware, while a value of 0 indicates the page is available
for allocation.

The PMM does **not** handle:

- Virtual memory mappings.
- Object-level allocation.
- Cache coherency or memory attributes.

Concurrency Safety and Spinlocks
================================
Because physical page allocation and deallocation can occur concurrently
across multiple CPU cores or within interrupt service routines (ISRs), all
shared PMM state is synchronized using a global spinlock: ``pmm_lock``.

Protected State
---------------
The ``pmm_lock`` spinlock serializes access to the following mutable state
variables:

- ``pmm_bitmap``: The underlying 64-bit word array representing page frame
  allocations.
- ``pmm_free_count``: The global count of currently unallocated page
  frames.
- ``pmm_last_alloc``: The search rotor position (PFN index) used for fast
  sequential allocations.

Scope of Synchronized Operations
--------------------------------
- **Allocations** (``pmm_alloc_page``, ``pmm_alloc_pages``,
  ``pmm_alloc_aligned``):
  Acquires ``pmm_lock``, scans the bitmap starting from the rotor
  (``pmm_last_alloc``), sets the corresponding allocation bits, updates
  statistics, advances the rotor, and releases the lock before returning
  the physical address.
- **Deallocations (``pmm_free_pages`` / ``pmm_free_page``):**
  Acquires ``pmm_lock``, validates the physical bounds, clears the bit
  flags, updates ``pmm_free_count``, and releases the lock.
- **Non-Locking Helpers:**
  ``pmm_alloc_page_zero()`` calls ``pmm_alloc_page()`` under the lock and
  performs the zeroing operation (via ``memset`` on the virtual address)
  **outside** the critical section, minimizing lock hold times.

Initialization Sequence
=======================
The PMM is the first memory subsystem initialized, while the boot page
tables (first 1 GiB only) are still active. ``pmm_init(mmap, mbi_start,
mbi_end)`` performs:

1. **Record RAM regions:** Copies every memory-map entry of type
   available, ACPI reclaimable, or ACPI NVS into ``pmm_regions[]`` (at
   most ``PMM_MAX_REGIONS``). The VMM builds the direct map from these
   (``pmm_region_count()`` / ``pmm_get_region()``).
2. **Size the bitmap:** One bit per page up to the highest *available*
   address, capped at ``PMM_MAX_MEMORY`` (1 TiB → 32 MiB bitmap).
3. **Place the bitmap:** In the first available region that has room
   below 1 GiB (it must be reachable through the boot window), sliding
   past the kernel image and the Multiboot2 info structure. Previously
   the bitmap was put immediately after the kernel without checking
   that the memory there was actually available.
4. **Default State:** Marks every page reserved (all bits set).
5. **Region Discovery:** Clears the bits for every available region.
6. **Critical Reservation:** Re-reserves exactly:

    - the low 1 MiB (IVT, BDA, EBDA, BIOS ROM, AP trampoline; page 0 is
      also the error sentinel);
    - the kernel image (``_kernel_physical_start`` ..
      ``_kernel_physical_end``, including the boot page tables and boot
      stack);
    - the Multiboot2 information structure;
    - the bitmap itself.

``vmm_init()`` then calls ``pmm_enable_direct_map()`` right after loading
the kernel's own page tables; from then on the bitmap is accessed through
the direct map and any frame can be zeroed.

Addressing and Conversion
=========================
The PMM operates exclusively on physical addresses (represented as
``uint64_t``). Because eMMK operates in a higher-half virtual address
space, addresses returned by the PMM cannot be dereferenced directly.

- After ``vmm_init()``: ``PHYS_TO_VIRT(paddr)`` (direct map) works for
  every RAM address.
- At any stage: ``pmm_phys_to_virt(paddr)`` uses the boot window
  (below 1 GiB only) before the direct map exists and the direct map
  afterwards. ``pmm_alloc_page_zero()`` uses it, and refuses (returns
  ``PMM_ALLOC_FAILED``) if an early allocation lands above 1 GiB.

.. code-block:: c

  // 1. Allocate a physical frame
  uint64_t paddr = pmm_alloc_page();

  if (paddr == PMM_ALLOC_FAILED)
  {
      panic("Out of physical memory!");
  }

  // 2. Convert to a virtual pointer before use
  void *ptr = PHYS_TO_VIRT(paddr);
  memset(ptr, 0, PAGE_SIZE);

Refer to ``include/kernel.h`` for the standard conversion macros
(``PHYS_TO_VIRT``, ``VIRT_TO_PHYS``) and :doc:`memory_layout` for where
everything resides.

Allocation API
==============

.. list-table:: **Primary API**
   :header-rows: 1
   :widths: 30 70

   * - Function
     - Description
   * - ``pmm_alloc_page()``
     - Allocates a single 4 KiB frame.
   * - ``pmm_alloc_page_zero()``
     - Allocates a single frame and clears it via ``memset``.
   * - ``pmm_alloc_pages(n)``
     - Allocates ``n`` contiguous 4 KiB frames.
   * - ``pmm_alloc_aligned(n, a)``
     - Allocates ``n`` contiguous, naturally-aligned page frames. The
       returned physical address is aligned to ``a`` * ``PAGE_SIZE``.
       ``pmm_alloc_aligned(512, 512)`` yields a 2 MiB block suitable for
       a large-page mapping.
   * - ``pmm_free_page(p)``
     - Releases a single frame at physical address ``p``.
   * - ``pmm_free_pages(p, n)``
     - Releases ``n`` contiguous frames starting at address ``p``.

Error Convention
================
If the PMM cannot satisfy an allocation request, it returns ``PMM_ALLOC_FAILED``
(defined as ``0``). Physical address ``0x0`` is never treated as a valid
allocation target, as the first page of RAM often contains legacy BIOS data
or the Interrupt Vector Table (IVT), making it an ideal sentinel value.

Range Reservation
=================
The PMM provides low-level functions to manually toggle the state of
memory ranges (``pmm_mark_used`` / ``pmm_mark_free``, both lock-protected).
This is primarily used by drivers to reserve specific hardware buffers
or by the kernel to protect bootloader-passed assets.

.. code-block:: c

  // Reserve a specific range for a hardware device
  pmm_mark_used(0x100000, 0x200000);

Diagnostics
===========
The PMM tracks the total and available memory via the ``pmm_stats`` structure.
This information is essential for memory pressure monitoring and kernel telemetry.

.. code-block:: c

  struct pmm_stats stats;
  pmm_get_stats(&stats);

  kprintf("Memory: %u/%u MiB free\n", 
          stats.free_memory / (1024 * 1024), 
          stats.total_memory / (1024 * 1024));

Design Tradeoffs
================
The bitmap allocator was chosen for eMMK due to its simplicity and high
metadata density. However, it presents specific performance characteristics:

- **Pros:** Constant time ``O(1)`` for single-page deallocation; highly
  compact metadata.
- **Cons:** Contiguous allocation (``pmm_alloc_pages``) requires a linear
  search through the bitmap, which can become ``O(n)`` (where ``n`` is
  the number of bits), potentially leading to performance degradation under
  heavy fragmentation. Fully allocated 64-page words are skipped whole,
  which keeps the scan fast while it is outside a candidate run.
  ``pmm_alloc_aligned`` always scans from PFN 0.

Neither cost matters on the data path: packet buffers come from slab
caches, which reach the PMM only when a cache grows.

References
==========

- `Multiboot2 Specification (GNU)
  <https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html>`_
  - §3.6.8 (memory map tag).
- `Intel SDM Vol. 3
  <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html>`_
  - §4 (Paging).
- `OSDev Wiki: Page Frame Allocation
  <https://wiki.osdev.org/Page_Frame_Allocation>`_
  - technical comparison of bitmap, buddy, and hybrid allocators.

Related Files
=============

- ``include/pmm.h``
- ``mm/pmm.c``
- ``include/kernel.h``
- ``include/spinlock.h``

