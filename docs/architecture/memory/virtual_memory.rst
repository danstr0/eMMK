======================
Virtual Memory Manager
======================

Overview
========
The Virtual Memory Manager (VMM) implements x86-64 4-level paging for eMMK.
It is responsible for translating virtual addresses into physical addresses,
enforcing memory protections (e.g., No-Execute), and managing the translation
lookaside buffer (TLB).

The VMM operates as a consumer of the physical memory. While the PMM
manages the supply of raw pages, the VMM provides the logic to organize
those pages into a coherent address space.

.. list-table:: **VMM Responsibilities**
   :widths: 30 70

   * - **Address Translation**
     - Managing ``PML4``, ``PDPT``, ``PD``, and ``PT`` hierarchies.
   * - **Access Control**
     - Enforcing Read/Write, User/Supervisor, and ``NX`` bit protections.
   * - **Caching Policy**
     - Configuring Write-Through or Cache-Disable (for MMIO).
   * - **TLB Coherency**
     - Ensuring CPU caches are invalidated when mappings change.


Address Space Layout
====================
eMMK utilizes a higher-half kernel design. The kernel is linked to and
resides in the top 2 GiB of the 48-bit virtual address space.

- **Kernel Base:** ``0xFFFF_FFFF_8000_0000``
- **Direct Map:** The bootloader establishes an initial 1 GiB
  identity-shifted mapping, allowing the kernel to access physical memory
  by adding the virtual base offset.


Page Table Hierarchy
====================
The x86-64 architecture uses a 4-level radix tree to resolve addresses.
Each level contains 512 entries, and each entry is 64 bits (8 bytes) wide,
fitting exactly into one 4 KiB page.

**Virtual Address Decomposition:**

.. code-block:: text

  │ Sign Extension │ PML4 Index │ PDPT Index │ PD Index │ PT Index │ Page Offset │
  │     [63:48]    │   [47:39]  │   [38:30]  │  [29:21] │  [20:12] │   [11:0]    │

When a mapping is requested for a virtual address, the VMM traverses this
tree. If an intermediate table (like a ``PD``) is missing, the VMM allocates
a new frame via ``pmm_alloc_page_zero()`` and links it into the hierarchy.


Mapping and Permissions
=======================
Mappings are created by modifying Page Table Entries (PTEs). These entries
contain the physical address of the target frame and a set of attribute flags.

.. list-table:: **PTE Attribute Flags**
   :header-rows: 1
   :widths: 20 20 60

   * - Flag
     - Bit
     - Description
   * - ``PRESENT``
     - 0
     - Page is valid and in RAM.
   * - ``WRITABLE``
     - 1
     - If clear, the page is Read-Only.
   * - ``USER``
     - 2
     - If clear, only Ring 0 (Kernel) can access.
   * - ``GLOBAL``
     - 8
     - Prevents TLB flush on CR3 reload.
   * - ``NX``
     - 63
     - No-Execute: Prevents instruction fetching.


Common Presets
==============
To simplify development, the VMM provides standard bitmasks for common
memory types:

- ``PTE_KERNEL_RW``: Present, Writable, No-Execute. (Standard data).
- ``PTE_KERNEL_ROX``: Present, Executable. (Kernel code).
- ``PTE_MMIO``: Present, Writable, Cache-Disable, No-Execute. (Device
  registers).

Mapping API
===========
The VMM provides a high-level API for managing address space.

.. code-block:: c

  // Map a 4 KiB page
  vmm_map_page(vaddr, paddr, PTE_KERNEL_RW);

  // Map a contiguous range (e.g., a buffer)
  vmm_map_pages(vaddr, paddr, count, PTE_KERNEL_RO);

  // Remove a mapping
  uint64_t freed_paddr = vmm_unmap_page(vaddr);
  pmm_free_page(freed_paddr);  // Explicitly release physical memory

  // Unmap a contiguous range
  vmm_unmap_pages(vaddr, count);

.. warning::

  Unmapping a page does **not** free the physical frame; memory management
  is decoupled to allow for shared memory and sophisticated caching.

TLB Management
==============
The Translation Lookaside Buffer (TLB) caches virtual-to-physical
translations. If the VMM modifies a PTE, the cached version in the CPU may
become "stale," leading to memory corruption.

The VMM supports three types of invalidation:

1. **Single Page (invlpg):** Invalidates a specific mapping. Used
   by ``vmm_unmap_page``.
2. **Full Flush (CR3 Reload):** Invalidates all non-global entries by
   reloading the Page Directory Base Register.
3. **Global Flush (CR4 Toggle):** By toggling the ``PGE`` (Page Global
   Enable) bit in ``CR4``, the VMM forces the CPU to flush even those
   entries marked with ``PTE_GLOBAL``.

.. note::
   Global flushes are expensive and should only be performed when modifying
   kernel-wide structures that affect all address spaces.


References
==========
- `Intel SDM Vol. 3
  <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html>`_
  - §4 (Paging).
- `AMD64 APM Vol. 2
  <https://docs.amd.com/v/u/en-US/40332_4.09_APM_PUB>`_
  - §5.6.3 (``NX`` bit), §15.16 (TLB control).
- `OSDev Wiki: Paging
  <https://wiki.osdev.org/Paging>`_
  - general guidance on paging and virtual memory.


Related Files
=============
- ``include/vmm.h``
- ``mm/vmm.c``
- ``include/kernel.h``

