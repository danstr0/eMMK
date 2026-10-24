============================================
Task State Segment (TSS) & Per-CPU Subsystem
============================================

Overview
========
In eMMK's 64-bit SMP architecture, CPU core isolation relies on two
low-level hardware/software constructs:

1. **Task State Segment (TSS):** Dedicated per-core hardware structures
   required by long mode for stack switching during interrupts, critical
   exceptions (via IST), and privilege changes.
2. **Per-CPU Data Areas (struct percpu):** A static, cache-line-aligned
   array of structures, one bound to each core through the x86-64 ``GS``
   segment base MSR (``MSR_GS_BASE`` = ``0xC0000101``).

Because eMMK operates exclusively in Ring 0 with a single flat address
space, these subsystems are optimized for zero-overhead access, eschewing
user-space switching mechanics like ``SWAPGS``.


Task State Segment (TSS) & Runtime GDT
======================================

64-Bit Mode TSS Context
-----------------------
In x86-64 long mode, hardware task switching is disabled. The 104-byte TSS
serves two remaining architectural functions:

* **Ring 0 Stack Switching (RSP0):** Provides the kernel stack pointer
  loaded when an interrupt transitions from lower privilege rings (unused
  for Ring 0 traps in eMMK, but required to be valid by the hardware).
* **Interrupt Stack Table (IST):** Offers up to 7 dedicated stack pointers
  (``IST1``–``IST7``) configured in IDT gates. This guarantees a clean,
  uncorrupted stack during severe faults (e.g., Double Fault ``#DF``) or
  asynchronous Non-Maskable Interrupts (``NMI``).

Memory Layout (104 Bytes)
~~~~~~~~~~~~~~~~~~~~~~~~~
The hardware layout of ``struct tss`` is strictly enforced via static
assertion:

.. list-table::
   :header-rows: 1
   :widths: 40 60 

   * - Offset
     - Field
   * - ``0x00`` - ``0x03``
     - Reserved0
   * - ``0x04`` - ``0x0B``
     - RSP0 (Ring 0 Stack)
   * - ``0x0C`` - ``0x1B``
     - RSP1 / RSP2 (Unused)
   * - ``0x1C`` - ``0x23``
     - Reserved1
   * - ``0x24`` - ``0x5B``
     - IST1 - IST7 (64-bit)
   * - ``0x5C`` - ``0x65``
     - Reserved2 / Reserved3
   * - ``0x66`` - ``0x67``
     - IOPB Base Offset

IST Allocation Policy
~~~~~~~~~~~~~~~~~~~~~
Each core allocates two independent 8 KiB stacks with ``vmm_alloc_stack()``
during initialization. They reside in the stack window, each with an
unmapped guard page below it (see :doc:`memory/memory_layout`):

* **IST1** (``IST_DOUBLE_FAULT``): Dedicated stack for vector 8 (``#DF``).
  Prevents kernel panics from escalating to triple faults when kernel
  stack overflow occurs.
* **IST2** (``IST_NMI``): Dedicated stack for vector 2 (``NMI``). Ensures
  clean execution during asynchronous non-maskable hardware interrupts.

Runtime GDT & System Descriptors
--------------------------------
The minimal 3-entry boot GDT created in ``multiboot2.asm`` is expanded at
runtime into ``runtime_gdt``. In 64-bit mode, TSS descriptors are extended
**System Descriptors** consuming 16 bytes (two consecutive 8-byte GDT
slots).

GDT Layout Map
~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1
   :widths: 30 10 10 50 

   * - GDT Index
     - Selector
     - Size
     - Description
   * - Slot 0
     - ``0x00``
     - 8 bytes
     - Null Descriptor
   * - Slot 1
     - ``0x08``
     - 8 bytes
     - 64-bit Ring 0 Code Segment
   * - Slot 2
     - ``0x10``
     - 8 bytes
     - 64-bit Ring 0 Data Segment
   * - Slot 3-4
     - ``0x18``
     - 16 bytes
     - TSS Descriptor (Core 0 / BSP)
   * - Slot 5-6
     - ``0x28``
     - 16 bytes
     - TSS Descriptor (Core 1 / AP)
   * - ...
     - ...
     - ...
     - ...
   * - Slot 2N+3
     - ``0x...``
     - 16 bytes
     - TSS Descriptor (Core N-1)

Selector Calculation
~~~~~~~~~~~~~~~~~~~~
For any core index :math:`C` (where BSP = 0):

.. code-block:: c

   #define GDT_BASE_ENTRIES  3
   #define GDT_TSS_SLOTS     2

   #define TSS_SELECTOR(core) \
       ((uint16_t)((GDT_BASE_ENTRIES + (core) * GDT_TSS_SLOTS) * 8))

Writing 64-Bit System Descriptors
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
A 16-byte TSS descriptor encodes the 64-bit virtual base address across
both low and high quads:

* **Low 64-bits:** Base[23:0], Base[31:24], Limit[19:0], Segment Type
  (``0x9`` = 64-bit Available TSS), Present flag (``P=1``), DPL=0.
* **High 64-bits:** Base[63:32] in bits [31:0]; top 32 bits reserved.


Per-CPU Architecture (``struct percpu``)
========================================

Hardware Mechanism (GS Base)
----------------------------
eMMK leverages the x86-64 ``MSR_GS_BASE`` (``0xC0000101``) model for
per-core variables. Each core writes the address of its own entry in the
static ``percpu_areas[SMP_MAX_CORES]`` array into the MSR.

Because eMMK runs purely in kernel space, no user-space context switches
occur, removing the need to execute ``SWAPGS``.

.. warning::

   In 64-bit mode, loading a selector into GS (``mov %ax, %gs``) also
   resets the GS base from the descriptor (to 0). Code that reloads
   segment registers after ``percpu_init()`` (``tss_init``, ``ap_entry``)
   must skip GS.

Static, Early, Cache-Line Aligned
---------------------------------
The areas are a static array rather than heap allocations:

- **Available from the first instruction:** The BSP calls
  ``percpu_init(0, 0)`` as the first statement of ``kernel_main()``; each
  AP calls ``percpu_init()`` at the top of ``ap_entry()`` with the core
  index the BSP passed it. ``this_core_id()`` is therefore valid
  everywhere.
- **No false sharing:** ``struct percpu`` is ``CACHE_ALIGNED``; two cores'
  areas never share a cache line. The heap-allocated version was 40 bytes
  with 16-byte alignment, so neighbors shared lines.
- **No failure path:** Nothing to allocate.

Self-Referential Pointer Pattern
--------------------------------
To achieve zero-overhead access without needing an extra register to hold
the base pointer, the first member of ``struct percpu`` is a pointer to
itself:

.. code-block:: c

   struct percpu
   {
       struct percpu *self;        // %gs:0 points here
       int            core_index;  // %gs:8
       uint32_t       apic_id;
       struct tss    *tss;
   } CACHE_ALIGNED;

Dereferencing `%gs:0`
~~~~~~~~~~~~~~~~~~~~~
Reading offset ``0`` from the ``GS`` segment returns the address stored in
``self``. This compiles down to a single assembly instruction:

.. code-block:: asm

   movq %gs:0, %rax   ; rax now holds current core's struct percpu*

Fast Field Offset Reads
~~~~~~~~~~~~~~~~~~~~~~~
Fields inside ``struct percpu`` can be read directly relative to ``GS``
without first loading the struct pointer. For instance, ``this_core_id()``
computes the member offset at compile time using ``__builtin_offsetof``:

.. code-block:: asm

   movl %gs:8, %eax   ; Reads core_index directly without materializing `self`

The accessors are *non-volatile* inline asm: a core's GS base never
changes after ``percpu_init()``, so the compiler may reuse a loaded value
instead of re-reading ``%gs``.


Initialization Lifecycle
========================

Boot Sequence Flow
------------------

.. code-block:: text

   [ BSP Initialization (kernel_main) ]
                   │
                   ▼
      1. percpu_init(0, 0)
         └── wrmsr(MSR_GS_BASE, &percpu_areas[0])
                   │
                   ▼
      2. pmm / vmm / heap / slab, interrupts, TSC
                   │
                   ▼
      3. tss_init()
         ├── Constructs runtime_gdt[]
         ├── Allocates guarded IST1/IST2 for Core 0
         ├── Populates TSS Slot 3-4
         ├── lgdt + reload CS, DS, ES, FS, SS (not GS)
         └── ltr (Selector 0x18)
                   │
                   ▼
      4. smp_init()  ──► [ AP Core Initialization (ap_entry) ]
         └── percpu_set_apic_id()     │
                                      ▼
                         1. cpu_init_core()
                         2. percpu_init(core_index, apic_id)
                            └── values from the trampoline parameter block
                         3. segments, IDT, LAPIC
                         4. tss_init_ap(core_index, stack_top)
                            ├── Allocates guarded IST1/IST2 for AP
                            ├── Populates TSS Descriptor
                            ├── lgdt (runtime_gdt_ptr)
                            └── ltr (TSS_SELECTOR(index))


Core API Reference
==================

TSS Functions
-------------

.. c:function:: void tss_init(void)

   Constructs the global ``runtime_gdt``, installs the BSP's TSS
   descriptor, reloads segment selectors (``CS=0x08``, ``DS/SS=0x10``),
   and issues ``ltr`` for core 0. Must be invoked on the BSP before
   initializing SMP.

.. c:function:: void tss_init_ap(int core_index, uint64_t stack_top)

   Initializes TSS and IST stacks for an Application Processor, writes its
   16-byte TSS descriptor into the shared ``runtime_gdt``, loads the
   runtime GDTR, and issues ``ltr``.

.. c:function:: struct tss *tss_get(int core_index)

   Returns a pointer to the ``tss`` structure for the specified core
   index, or ``NULL`` if out of bounds.

Per-CPU Functions & Accessors
-----------------------------

.. c:function:: void percpu_init(int core_index, uint32_t apic_id)

   Fills in ``percpu_areas[core_index]`` and points the calling core's
   ``MSR_GS_BASE`` at it. Needs nothing else initialized.

.. c:function:: void percpu_set_apic_id(uint32_t apic_id)

   Updates the calling core's recorded APIC ID (used by ``smp_init()``
   for the BSP).

.. c:function:: struct percpu *percpu_get(int core_index)

   Another core's area (or ``NULL`` if out range), for cross-core
   inspection.

.. c:function:: static inline struct percpu *this_cpu(void)

   Retrieves the current core's ``struct percpu`` pointer via
   ``movq %gs:0``.

.. c:function:: static inline int this_core_id(void)

   Retrieves the current core index directly via offset loading
   (``movl %gs:<offset>``).

References
==========

- `Intel SDM Vol. 3
  <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html>`_
  — §6.14.5 (Interrupt Stack Table), §8.2 (Task Management Data
  Structures).
- `AMD64 Architecture Programmer's Manual Vol. 2
  <https://www.amd.com/en/search/documentation/hub.html>`_
  — §12.2 (TSS).


Related Files
=============

- ``include/tss.h`` / ``kernel/tss.c``
- ``include/percpu.h`` /  ``kernel/percpu.c``
- ``include/cpu.h``
- ``test/test_smp.c``

