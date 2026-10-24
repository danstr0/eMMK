======================
Virtual Memory Manager
======================

Overview
========
The Virtual Memory Manager (VMM) implements x86-64 4-level paging for eMMK.
It builds the kernel's page tables at boot, maintains the direct map of
physical memory, maps device registers uncached, and provides the
map/unmap/query API for everything else.

The VMM operates as a consumer of the physical memory. While the PMM
manages the supply of raw pages, the VMM provides the logic to organize
those pages into a coherent address space.

.. list-table:: **VMM Responsibilities**
   :widths: 30 70

   * - **Address Translation**
     - Managing ``PML4``, ``PDPT``, ``PD``, and ``PT`` hierarchies, with
       4 KiB, 2 MiB, and 1 GiB pages.
   * - **Access Control**
     - Read/Write and ``NX`` per kernel section; unmapped NULL and guard
       pages..
   * - **Caching Policy**
     - Write-back for RAM; strong uncacheable (PCD|PWT) for MMIO.
   * - **TLB Coherency**
     - Local ``invlpg`` after every change (no cross-core shootdown).


Address Space Layout
====================
See :doc:`memory_layout` for the full map. In short:

- **Kernel image** at ``KERNEL_VIRTUAL_BASE`` (-2 GiB) + physical address,
  with per-section permissions.
- **Direct map** at ``DIRECT_MAP_BASE`` (``0xFFFF_8000_0000_0000``):
  every RAM region, so ``PHYS_TO_VIRT(p)`` = ``DIRECT_MAP_BASE + p``.
- **Heap** at ``HEAP_VIRTUAL_BASE``, so **MMIO window** at
  ``MMIO_VIRTUAL_BASE``.
- **Identity map** of ``[4 KiB, 2 MiB)`` for AP startup only.


Bootstrapping with vmm_init()
=============================
The boot code (``multiboot2.asm``) leaves a minimal set of tables: the
first 1 GiB, identity-mapped and mirrored at -2 GiB, with 2 MiB pages,
RWX, PML4[0]/[511] sharing one PDPT. ``vmm_init()`` replaces them:

1. Allocates a fresh PML4. Every table frame allocated during init is
   below 1 GiB, and is written through the boot window
   (``pmm_phys_to_virt``).
2. Maps the kernel image section by section, using linker symbols
   (``__text_start``, ...):

   .. list-table::
      :header-rows: 1
      :widths: 30 20 50

      * - Section
        - Permissions
        - Notes
      * - ``.boot``
        - RX
        - 32-bit entry, boot GDT
      * - ``.boot_page_tables``
        - *unmapped*
        - Guard page below the boot stack
      * - ``.boot_stack``
        - RW, NX
        - BSP stack
      * - ``.ap_trampoline``
        - RO, NX
        - Source copy of the trampoline
      * - ``.text``
        - RX
        -
      * - ``.rodata``
        - RO, NX
        -
      * - ``.data`` ... end
        - RW, NX
        - ``.data``, ``.bss``

3. Identity-maps ``[4 KiB, 2 MiB)`` for the AP trampoline. Page 0 stays
   unmapped, so NULL dereferences fault instead of reading the real-mode
   IVT.
4. Maps every PMM RAM region into the direct map with ``vmm_map_range()``.
5. Loads CR3, then calls ``pmm_enable_direct_map()``.
6. Calls ``cpu_init_core()``, which sets **CR0.WP**. Without it, ring-0
   writes ignore the read-only bit, so none of the permissions above would
   bind kernel code (all eMMK code). APs do the same in ``ap_entry()``.

The switch is safe because RIP (``.text``), RSP (boot stack), and the
GDT (identity) are mapped at the same address in both table sets.
Afterwards, the boot window is gone: only the kernel image remains
mapped at -2 GiB.


Page Table Hierarchy
====================
The x86-64 architecture uses a 4-level radix tree to resolve addresses.
Each level contains 512 entries, and each entry is 64 bits (8 bytes) wide,
fitting exactly into one 4 KiB page.

**Virtual Address Decomposition:**

.. code-block:: text

  │ Sign Extension │ PML4 Index │ PDPT Index │ PD Index │ PT Index │ Page Offset │
  │     [63:48]    │   [47:39]  │   [38:30]  │  [29:21] │  [20:12] │   [11:0]    │

A PDPT entry with PS=1 maps a 1 GiB page; a PD entry with PS=1 maps a
2 MiB page. When a mapping needs a missing intermediate table, the VMM
allocates one with ``pmm_alloc_page_zero()``. Intermediate entries are
always present+writable; permissions are decided at the leaf.


Mapping and Permissions
=======================

.. list-table:: **PTE Attribute Flags**
   :header-rows: 1
   :widths: 20 20 60

   * - Flag
     - Bit
     - Description
   * - ``PRESENT``
     - 0
     - Page is valid.
   * - ``WRITABLE``
     - 1
     - If clear, the page is read-only.
   * - ``WRITE_THROUGH``
     - 3
     - PWT (PAT index bit 0).
   * - ``CACHE_DISABLE``
     - 4
     - PCD (PAT index bit 1).
   * - ``HUGE``
     - 7
     - PS: 2 MiB (PD) or 1 GiB (PDPT) page.
   * - ``GLOBAL``
     - 8
     - Prevents TLB flush on CR3 reload (unused).
   * - ``NX``
     - 63
     - No-Execute.

Presets: ``PTE_KERNEL_RW`` (data), ``PTE_KERNEL_RO``, ``PTE_KERNEL_ROX``
(code), ``PTE_KERNEL_RWX``, and ``PTE_MMIO`` (RW, NX, PCD|PWT — strong UC
under the power-on PAT).


Mapping API
===========

.. code-block:: c

  // 4 KiB pages
  vmm_map_page(vaddr, paddr, PTE_KERNEL_RW);
  vmm_map_pages(vaddr, paddr, count, PTE_KERNEL_RO);  // rolls back on failure

  // A contiguous range with the largest page sizes that fit
  vmm_map_range(vaddr, paddr, size, PTE_KERNEL_RW);

  // Device registers: fresh uncached mapping in the MMIO window
  volatile uint32_t *regs = vmm_map_mmio(bar_phys, bar_size);

  // A kernel stack with an unmapped guard page below (returns the top)
  uint64_t rsp = vmm_alloc_stack(16 * 1024);

  // After SMP bring-up: drop the [4K, 2M) identity map
  vmm_unmap_identity();

  // Remove a mapping
  uint64_t freed_paddr = vmm_unmap_page(vaddr);
  pmm_free_page(freed_paddr);  // Explicitly release physical memory

``vmm_map_page`` returns ``VMM_ERR_INVALID`` if a large page already
covers the address, and ``vmm_map_range`` refuses to replace an existing
page table with a large leaf (``VMM_ERR_ALREADY``).

``vmm_map_mmio`` never reuses virtual space and leaves a guard page after
each region. Every caller gets its own mapping.


.. warning::

  Unmapping a page does **not** free the physical frame; memory management
  is decoupled to allow for shared memory and sophisticated caching.


Queries and Translation
=======================
- ``vmm_get_physical(vaddr)`` — page table walk, any page size.
- ``vmm_is_mapped(vaddr)``, ``vmm_get_flags(vaddr)`` — the leaf entry's
  flags (0 if unmapped).
- ``virt_to_phys(ptr)`` — arithmetic for direct-map and kernel-image
  addresses, a walk for anything else (heap, MMIO, ad-hoc mappings).
- ``VIRT_TO_PHYS_ADDR(v)`` (kernel.h) — arithmetic only; valid for
  direct-map and kernel-image addresses. Use this for hot paths with
  slab / PMM memory.


Concurrency and TLB Management
==============================
All modifications take an internal spinlock (with interrupts disabled);
queries do not. The VMM calls into the PMM while holding its lock.

Every modification flushes the local TLB entry with ``invlpg``.
**There is no TLB shootdown**: after ``smp_init()``, changing or removing
a mapping that other cores may have cached leaves them with a stale
translation. Creating new mappings is safe, because x86 does not cache
not-present translations. At the moment all post-SMP mapping activity is
creation (heap growth, MMIO), so this is a constraint on future code
rather than a live bug.

``vmm_flush_tlb_all()`` reloads CR3 (non-global entries only).


References
==========
- `Intel SDM Vol. 3
  <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html>`_
  - §4 (Paging), §4.10 (TLBs and paging-structure caches), PAT.
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
- ``kernel/linker.ld``
- ``test/test_vmm.c``

