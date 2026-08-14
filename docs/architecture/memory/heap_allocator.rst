==============
Heap Allocator
==============

Overview
========
The eMMK kernel heap provides a general-purpose, variable-sized memory
allocation service similar to the standard C ``malloc`` / ``free``. It is
used for dynamic data structures (e.g., linked lists, buffers, and driver
state) that do not fit the fixed-size constraints of a slab allocator or
the 4 KiB granularity of the vmm.

.. list-table:: **Allocator Properties**
   :widths: 30 70

   * - **Algorithm**
     - First-fit linked list with block coalescing.
   * - **Alignment**
     - 16-byte (compatible with x86-64 ABI and SSE).
   * - **Backing Store**
     - Virtual memory mapped via the vmm using physical-memory
       frames.


Implementation Strategy
=======================
The heap is implemented as a linked list of free blocks. Each block in
the heap (both allocated and free) is preceded by a metadata header that
tracks the block's size and its status.

- **Allocation (kmalloc):** The allocator performs a first-fit search of
  the free list. When a block of sufficient size is found, it is "split"
  (if the remainder is large enough to form a new block) and marked as
  allocated.
- **Deallocation (kfree):** The block is marked as free. To prevent
  fragmentation, the allocator immediately checks the physically adjacent
  neighbors; if they are also free, they are coalesced into a single,
  larger contiguous block.
- **Growth:** If the heap cannot satisfy an allocation, it requests new
  pages from the VMM/PMM to expand the heap's end boundary, up to a maximum
  of 16 MiB.


Memory Layout and Alignment
===========================
All pointers returned by the heap are guaranteed to be aligned to 16 bytes.
This is critical for:

- **x86-64 ABI Compliance:** The stack and many structures expect 16-byte
  alignment.
- **SIMD Operations:** Instructions like ``movaps`` (SSE) require 16-byte
  alignment to avoid General Protection Faults.

Any request smaller than 32 bytes (``HEAP_MIN_ALLOC``) is automatically
rounded up to ensure metadata overhead does not excessively fragment the heap.


Core API
========

.. list-table:: **Allocation Functions**
   :header-rows: 1
   :widths: 30 70

   * - Function
     - Description
   * - ``kmalloc(size)``
     - Allocates ``size`` bytes of uninitialized memory.
   * - ``kzalloc(size)``
     - Allocates ``size`` bytes and zeroes the memory. Preferred for
       structures.
   * - ``krealloc(ptr, size)``
     - Resizes an existing allocation, moving it if necessary.
   * - ``kaligned_alloc(size, align)``
     - Allocates memory with a specific power-of-two alignment.
   * - ``kfree(ptr)``
     - Returns the memory at ``ptr`` to the heap.


Usage Guidelines
================

Type-Safe Allocation
--------------------
To avoid manual ``sizeof`` calculations and casting, use the provided
convenience macros:

.. code-block:: c

  // Allocate and zero a single structure
  struct pci_device *dev = kzalloc_type(struct pci_device);

  // Allocate an array with overflow checking
  struct my_struct *arr = kzalloc_array(10, sizeof(struct my_struct));


Error Handling
--------------
All heap allocation functions return ``NULL`` on failure. Kernel code
**must** check for this condition to prevent null-pointer dereferences
during memory pressure.

.. code-block:: c

  void *buf = kmalloc(4096);
  if (!buf)
  {
      return;
  }


Diagnostics and Debugging
-------------------------
The allocator maintains lifetime statistics to help detect memory leaks
and fragmentation issues.

- ``heap_get_stats()``: Populates a ``heap_stats`` structure with current
  usage, lifetime allocation counts, and the number of free blocks.
- ``heap_dump()``: Iterates through the entire free list and prints block
  addresses and sizes to the `:ref:serial-console`. This is the primary
  tool for diagnosing heap corruption.


References
==========
- `System V Application Binary Interface
  <https://gitlab.com/x86-psABIs/x86-64-ABI>`_
  - §3.7.3.1 (stack alignment).


Related Files
=============
- ``kernel/heap.h``
- ``mm/heap.c``

