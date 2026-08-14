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

- Virtual memory mappings (see :ref:vmm).
- Object-level allocation (see :ref:slab-allocator).
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
During the early boot phase, the PMM must be initialized before the kernel
can enable paging or start complex subsystems. The ``pmm_init()`` function
performs the following sequence:

1. **Parse Multiboot2 Map:** Scans the memory map provided by the bootloader
   to identify usable RAM regions and "holes" (MMIO, reserved firmware
   space).
2. **Bitmap Placement:** Calculates the required bitmap size and places it
   in physical memory immediately following the kernel's binary image.
3. **Default State:** Initially marks the entire address space as Reserved
   (all bits set to 1).
4. **Region Discovery:** Iterates through the Multiboot2 map and marks all
   "Available" RAM regions as Free (bits cleared to 0).
5. **Critical Reservation:** Explicitly re-reserves the following to
   prevent accidental overwriting:

    - The kernel image (``text``, ``data``, ``rodata``, ``bss``).
    - The PMM bitmap itself.
    - Multiboot2 information structures and module payloads.
    - Physical page 0 (used as a sentinel/error value).

Addressing and Conversion
=========================
The PMM operates exclusively on physical addresses (represented as
``uint64_t``). Because eMMK operates in a higher-half virtual address
space, addresses returned by the PMM cannot be dereferenced directly.

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
(``PHYS_TO_VIRT``, ``VIRT_TO_PHYS``).

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
memory ranges. This is primarily used by drivers to reserve specific
hardware buffers or by the kernel to protect bootloader-passed assets.

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
  heavy fragmentation.

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

