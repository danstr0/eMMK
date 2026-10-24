=============
Memory Layout
=============

Overview
========
This page is the map of where everything resides, physically and virtually,
once ``kernel_main()`` has finished initializing memory — i.e., the state
an application built on eMMK starts from. 

The example values below come from the default kernel on QEMU with
128 MiB (``make run``). Addresses past the kernel image depend on
allocation order and will shift as the kernel changes; the *structure*
does not.


Physical Memory
===============

Firmware / bootloader view
--------------------------
QEMU's memory map (Multiboot2 tag 6) for ``-m 128M``:

.. list-table::
   :header-rows: 1
   :widths: 30 20 50

   * - Range
     - Type
     - Notes
   * - ``0x0000_0000 .. 0x0009_FBFF``
     - available
     - Conventional memory (639 KiB)
   * - ``0x0009_FC00 .. 0x0009_FFFF``
     - reserved
     - EBDA
   * - ``0x000F_0000 .. 0x000F_FFFF``
     - reserved
     - BIOS ROM
   * - ``0x0010_0000 .. 0x07FD_FFFF``
     - available
     - Main RAM
   * - ``0x07FE_0000 .. 0x07FF_FFFF``
     - reserved
     - ACPI tables (QEMU)
   * - ``0xFFFC_0000 .. 0xFFFF_FFFF``
     - reserved
     - Firmware flash
   * - ``0xFD_0000_0000 ..``
     - reserved
     - QEMU-internal

With more than ~3 GiB, QEMU splits RAM around the 32-bit PCI hole: RAM
continues at ``0x1_0000_0000`` (4 GiB). The PMM and the direct map
handle any number of RAM regions (up to ``PMM_MAX_REGIONS``).

What eMMK puts where
--------------------

.. list-table::
   :header-rows: 1
   :widths: 28 22 50

   * - Physical range
     - Owner
     - Contents
   * - ``0x0000 .. 0x0FFF``
     - reserved
     - Real-mode IVT. Never allocated (also the PMM's error sentinel).
   * - ``0x8000 .. 0x8FFF``
     - SMP
     - AP trampoline (copied here by ``smp_init``); parameter block at
       ``0x8F00``
   * - ``0x0 .. 0xF_FFFF``
     - reserved
     - The whole low 1 MiB is reserved by the PMM
   * - ``0x10_0000 .. 0x10_0FFF``
     - kernel image
     - ``.boot``: Multiboot2 header, 32-bit entry, boot GDT
   * - ``0x10_1000 .. 0x10_3FFF``
     - kernel image
     - ``.boot_page_tables``: boot PML4/PDPT/PD. Dead after
       ``vmm_init()``; unmapped, serving as the boot stack's guard page
   * - ``0x10_4000 .. 0x10_7FFF``
     - kernel image
     - ``.boot_stack``: the BSP's 16 KiB stack, for the kernel's lifetime
   * - ``0x10_8000 .. 0x10_8FFF``
     - kernel image
     - ``.ap_trampoline`` (source copy of the trampoline)
   * - ``0x10_9000 .. 0x12_2137``
     - kernel image
     - ``.text``, ``.rodata``, ``.data``, ``.bss``
   * - (GRUB's choice)
     - reserved
     - Multiboot2 information structure
   * - ``0x12_3000 ..`` (example)
     - PMM
     - Bitmap: 1 bit per 4 KiB page up to the highest RAM address
       (4 KiB per 128 MiB; 32 MiB at the 1 TiB maximum). Placed in free
       RAM below 1 GiB, clear of the kernel and the MBI
   * - everything else
     - PMM free pool
     - See *Consumers of the free pool* below
   * - ``0xFEC0_0000``
     - device
     - IOAPIC registers (MMIO, not RAM)
   * - ``0xFEE0_0000``
     - device
     - Local APIC registers (MMIO, not RAM; same address on every core)
   * - PCI BARs
     - devices
     - e.g. virtio-net MSI-X table at ``0xFEB9_1000`` (MMIO, not RAM)

Consumers of the free pool
--------------------------

.. list-table::
   :header-rows: 1
   :widths: 25 22 15 13 25

   * - Consumer
     - Allocator
     - Contiguous
     - DMA-safe
     - Notes
   * - Kernel page tables
     - ``pmm_alloc_page_zero``
     - 4 KiB
     - n/a
     - First ones allocated below 1 GiB during ``vmm_init``
   * - Heap
     - PMM, one page at a time
     - **No**
     - **No**
     - 64 KiB initially, up to 16 MiB
   * - Slab caches
     - ``pmm_alloc_aligned``
     - Per slab (4–32 KiB)
     - Yes
     - Includes netbuf structs and 2 KiB packet buffers
   * - AP stacks
     - ``vmm_alloc_stack``
     - **No**
     - n/a
     - 16 KiB, guard page below
   * - IST stacks
     - ``vmm_alloc_stack``
     - **No**
     - n/a
     - 8 KiB, two per core (#DF, NMI), guard page below
   * - virtio rings
     - PMM
     - Yes
     - Yes
     - Shared with the device
   * - virtio RX buffers
     - PMM
     - Yes
     - Yes
     - Driver-private pool (copied into netbufs at the moment)


Virtual Address Space
=====================

.. list-table::
   :header-rows: 1
   :widths: 35 15 50

   * - Range
     - PML4 slot
     - Contents
   * - ``0x0000_0000_0000_0000``
     - 0
     - Unmapped — NULL dereferences fault
   * - ``0x0000_0000_0000_1000 .. 0x0000_0000_001F_FFFF``
     - 0
     - Identity map (RWX, 4 KiB pages), **only during SMP bring-up**:
       APs execute the trampoline and load the boot GDT at physical
       addresses. Removed at the end of ``smp_init()``; afterwards the
       whole lower half is unmapped
   * - ``0xFFFF_8000_0000_0000 ..``
     - 256+
     - **Direct map**: physical address *p* at ``DIRECT_MAP_BASE + p``,
       for every RAM region. RW+NX, write-back, largest pages that fit
       (1 GiB if supported, else 2 MiB; 4 KiB at ragged edges).
       ``PHYS_TO_VIRT`` / ``VIRT_TO_PHYS``
   * - ``0xFFFF_C000_0000_0000 ..``
     - 384
     - **Heap** (``kmalloc``), up to 16 MiB
   * - ``0xFFFF_D000_0000_0000 ..``
     - 416
     - **Stack window**: ``vmm_alloc_stack()`` — AP stacks and IST stacks,
       each preceded by an unmapped guard page
   * - ``0xFFFF_E000_0000_0000 ..``
     - 448
     - **MMIO window**: ``vmm_map_mmio()`` mappings, uncached (PCD|PWT),
       each followed by an unmapped guard page
   * - ``0xFFFF_FFFF_8010_0000 ..``
     - 511
     - **Kernel image** (``KERNEL_VIRTUAL_BASE`` + physical), per-section
       permissions:

       - ``.boot`` RX; boot page tables **unmapped** (guard);
         ``.boot_stack`` RW+NX; ``.ap_trampoline`` RO+NX
       - ``.text`` RX; ``.rodata`` RO+NX; ``.data`` / ``.bss`` RW+NX

The kernel image is mapped with 4 KiB pages (it starts at 1 MiB, which is
not 2 MiB-aligned, and per-section permissions need page granularity).
Because nothing maps the legacy region below 1 MiB with a large page any
more, no page spans memory of different MTRR types.


Memory Initialization Sequence
==============================

.. code-block:: text

   entry64.asm    boot tables: identity + -2 GiB window, first 1 GiB, 2 MiB pages
        │
   pmm_init()     records RAM regions, places + fills the bitmap
        │         (accessed through the boot window)
        ▼
   vmm_init()     builds new tables (frames below 1 GiB, via the boot window)
        │         kernel image · identity [4K, 2M) · direct map
        │         mov cr3  ──►  pmm_enable_direct_map()
        ▼
   heap_init()    maps heap pages at HEAP_VIRTUAL_BASE
   slab_init()    size-class caches
        │
   lapic_init(), ioapic_init(), pci MSI-X   ──►  vmm_map_mmio()

After ``vmm_init()`` the boot window is gone: any pointer derived from it
(such as the Multiboot2 info pointer passed to ``kernel_main``) must be
re-derived through ``PHYS_TO_VIRT``.


Large Pages and Future Hugepage Use
===================================
- The direct map already uses 2 MiB (or 1 GiB) pages, so slab objects,
  packet buffers, and virtio rings — all accessed through it — cost few
  TLB entries.
- ``pmm_alloc_aligned(512, 512)`` returns a 2 MiB-aligned 2 MiB block,
  and ``vmm_map_range()`` maps it with a single large page. These are the
  building blocks for dedicated hugepage packet-buffer pools.
- QEMU's default CPU model lacks 1 GiB page support.


Constraints and Real-Hardware Notes
===================================
- **Boot window:** Until ``vmm_init()`` switches tables, only the first
  1 GiB is reachable. The Multiboot2 info, the PMM bitmap, and the new
  page tables' first frames must therefore lie below 1 GiB.
  ``TODO``: GRUB usually guarantees this for the MBI, but nothing
  enforces it.
- **AP trampoline:** The trampoline loads CR3 in 32-bit mode, so the
  kernel PML4 must be below 4 GiB (it is: allocated early, below 1 GiB).
- **No TLB shootdown:** Changing or removing a mapping after APs are up
  only flushes the local TLB. Creating mappings is safe.
- **Stacks:** Every kernel stack has a guard page: the BSP boot stack
  (the dead boot page tables below it) and all AP and IST stacks (stack
  window).
- **Write protection:** ``CR0.WP`` is set on every core
  (``cpu_init_core()``), so read-only mappings bind kernel code.


Related Files
=============
- ``include/kernel.h`` (layout constants, conversions)
- ``kernel/linker.ld`` (image layout and section symbols)
- ``mm/pmm.c``, ``mm/vmm.c``, ``mm/heap.c``, ``mm/slab.c``

