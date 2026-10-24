================================
Symmetric Multi-Processing (SMP)
================================

Overview
========

eMMK initializes Application Processors (APs) after the Bootstrap
Processor (BSP) completes basic subsystem setup (memory, IDT, LAPIC, TSC
clock, TSS). APs are discovered, brought out of real mode via an
INIT-SIPI-SIPI sequence, guided through long-mode activation by a
low-memory assembly trampoline, and transitioned into a per-core run loop
in C. Startup needs no interrupts on the BSP: all delays use the TSC.


Core Discovery
==============
The number of logical processors comes from CPUID leaf 1 (``EBX[23:16]``),
and AP *i* is assumed to have APIC ID *i*.

``TODO``: on real hardware, the processor list must come from the ACPI
MADT.

Core Indices and Failure Handling
---------------------------------
Core indices are dense: the BSP is 0, and each AP that starts gets the
next free index, which the BSP passes in the parameter block
(``core_index``). If an AP does not report ready within 100 ms, the BSP
sends it an INIT IPI — parking it back in wait-for-SIPI — and reuses the
index for the next AP. 

.. list-table:: **AP Startup Stages**
   :header-rows: 1
   :widths: 10 30 30

   * - Stage
     - Location
     - Execution Context
   * - 1
     - Real-mode entry (``ap_trampoline.asm``)
     - 16-bit real mode (phys ``0x8000``)
   * - 2
     - Protected-mode setup (``ap_trampoline.asm``)
     - 32-bit protected mode
   * - 3
     - Long-mode entry (``ap_trampoline.asm``)
     - 64-bit long mode (identity VMA)
   * - 4
     - C Runtime Setup & Loop (``smp.c`` → ``ap_entry()``)
     - 64-bit long mode, per-core stack & GS base


Initialization Flow
===================

::

    BSP (kernel_main)
     │
     │  1. setup_trampoline() [copy code to 0x8000, write ap_boot_params]
     │  2. vmm_alloc_stack()  [16 KiB per AP, guard page below]
     │  3. write stack_top, ap_id, core_index
     │
     ├─── Send INIT IPI
     │    Wait 10 ms
     ├─── Send SIPI (vector 0x08)
     │    Delay 200 µs
     ├─── Send SIPI (vector 0x08)
     │
     ├────────────────────▶ AP (Real Mode @ 0x8000)                [ap_trampoline.asm]
     │                       ├── Fast A20 enable (port 0x92)
     │                       ├── Load temporary GDT (gdt32)
     │                       └── Enable CR0.PE
     │                           │
     │                           ▼
     │                           32-bit Protected Mode
     │                           ├── Load PML4 into CR3
     │                           ├── Enable CR4.PAE & EFER.LME/NXE
     │                           ├── Enable CR0.PG
     │                           ├── Load 64-bit GDT (from BSP)
     │                           └── Far jump
     │                               │
     │                               ▼
     │                               64-bit Long Mode
     │                               ├── Load RSP from params
     │                               └── jmp ap_entry
     │                                   │
     │  Wait for ap_ready                ▼
     │◄────────────────────────────── ap_entry() [smp.c]
     │                                ├── cpu_init_core() (CR0.WP, SSE bits)
     │                                ├── percpu_init(core_index) (GS base)
     │                                ├── Reload DS/ES/FS/SS (not GS)
     │                                ├── Load IDT (lidt)
     │                                ├── Enable LAPIC & TPR
     │                                ├── tss_init_ap() (runtime GDT, TR)
     │                                ├── Enable Interrupts (sti)
     │                                ├── Signal online & ap_ready=1
     │                                └── HLT loop (await launch)
     │
     │ (all APs started)
     ├─ vmm_unmap_identity()
     ▼
  smp_launch_all()
     │
     └── launched = true + wake IPI ──▶ flush TLB, execute run_func()


Trampoline Page Layout
======================

The trampoline is copied by the BSP to ``AP_TRAMPOLINE_ADDR`` (``0x8000``),
a page-aligned physical address below 1 MiB. Parameter passing occurs via
a dedicated block at offset ``0xF00`` within the same 4 KiB page to avoid
inline code mutation.

.. list-table:: **Trampoline Page Structure (4 KiB at 0x8000)**
   :header-rows: 1
   :widths: 15 25 40

   * - Offset
     - Name
     - Description
   * - ``0x000``
     - ``ap_trampoline_start``
     - Real mode entry point (16-bit).
   * - ``0x...``
     - ``protected_entry``
     - 32-bit entry, page table loading, MSR configuration.
   * - ``0x...``
     - ``long_mode_entry``
     - 64-bit entry, stack initialization, jump to C.
   * - ``0xE00``
     - Temp Stack
     - Temporary 32-bit stack (used during GDT reload).
   * - ``0xF00``
     - ``ap_boot_params``
     - Shared parameters written by BSP before SIPI.

Boot Parameters Structure
-------------------------
The BSP populates ``struct ap_boot_params`` at address ``0x8F00`` prior to
issuing IPIs:

.. list-table::
   :header-rows: 1
   :widths: 15 15 45

   * - Offset
     - Field
     - Value / Purpose
   * - ``+0x00``
     - ``pml4_addr``
     - Physical address of PML4 (copied from BSP's ``CR3``).
   * - ``+0x04``
     - ``gdt_ptr_low``
     - Linear address of 64-bit GDT (low 32 bits).
   * - ``+0x08``
     - ``gdt_ptr_high``
     - Linear address of 64-bit GDT (high 32 bits).
   * - ``+0x0C``
     - ``gdt_limit``
     - 16-bit GDT limit (size - 1).
   * - ``+0x10``
     - ``entry_point``
     - 64-bit virtual address of ``ap_entry()``.
   * - ``+0x18``
     - ``stack_top``
     - Virtual address of per-AP stack top (16 KiB, guarded).
   * - ``+0x20``
     - ``ap_ready``
     - Set to ``1`` by the AP once fully initialized (TSS, per-CPU).
   * - ``+0x24``
     - ``ap_id``
     - APIC ID of the AP being started (read by ``ap_entry``).
   * - ``+0x28``
     - ``core_index``
     - Core index assigned by the BSP (read by ``ap_entry``).

The trampoline reads the fields up to ``stack_top``; ``ap_entry`` reads
the rest. APs start strictly one at a time (the BSP waits for
``ap_ready``), so a single block serves them all.

Identity Map Lifetime
---------------------
The trampoline runs at physical addresses with paging enabled, and the
boot GDT it loads has a physical base, so APs need the ``[4 KiB, 2 MiB)``
identity map until ``tss_init_ap()`` switches them to the runtime GDT.
Once every AP has reported ready, ``smp_init()`` removes the identity map
(``vmm_unmap_identity()``); each AP reloads CR3 when it is launched, so no
stale identity translation survives in any TLB.


AP States
=========
Each AP progresses through four hardware and software execution states
during startup:

.. list-table::
   :header-rows: 1
   :widths: 20 20 25 25

   * - Mode
     - Addressing
     - Code / File
     - Key Actions / Registers
   * - **16-bit Real**
     - Real (Segment:Offset)
     - ``ap_trampoline.asm``
     - Enable Fast A20 (port ``0x92``), ``lgdt [gdt32]``, set ``CR0.PE``.
   * - **32-bit Protected**
     - Flat 32-bit Linear
     - ``ap_trampoline.asm``
     - Set ``CR3``, enable ``CR4.PAE``, set ``EFER.LME``/``NXE``, set
       ``CR0.PG``.
   * - **64-bit Compatibility**
     - Paged (32-bit segment)
     - ``ap_trampoline.asm``
     - Load 64-bit GDT pointer, far jump into 64-bit code segment
       (``0x08``).
   * - **64-bit Long Mode**
     - Paged 64-bit Virtual
     - ``smp.c`` (``ap_entry``)
     - Load ``RSP``; in ``ap_entry``: control registers, ``GS_BASE``, IDT,
       LAPIC, TSS; issue ``sti``.

Every AP gets the same control-register state as the BSP via
``cpu_init_core()``: **CR0.WP** (ring-0 writes honor read-only pages)
and the SSE enable bits.


Per-Core Identification & Storage
=================================
Core numbering follows a dense index scheme ``0 .. smp_get_core_count()-1``,
in which Core 0 is strictly reserved for the BSP.

``smp_core_id()`` is a single ``%gs``-relative load of the per-CPU
``core_index`` (see :doc:`tss_percpu`). It is valid from the first
statement of ``kernel_main()`` / ``ap_entry()``.

Stacks
------
Each AP's 16 KiB stack and every core's two IST stacks come from
``vmm_alloc_stack()``: mapped in the stack window with an unmapped guard
page below. A stack overflow faults on the guard page; the page fault
cannot push its frame, escalates to #DF, and is reported from the #DF IST
stack.


Work Assignment & Dispatch
==========================
1. **Discovery & Hold:** ``smp_init()`` brings all APs through long mode
   transition. Upon entering ``ap_entry()``, each AP sets its ``online``
   flag, sets ``ap_boot_params.ap_ready = 1``, and halts waiting for
   ``cores[i].launched``. The flag is checked with interrupts disabled and
   the AP sleeps with ``sti; hlt``, so a wake IPI arriving between the
   check and the ``hlt`` still wakes it.
2. **Role Assignment:** The BSP calls
   ``smp_assign_role(core_index, func, name)`` to set a per-core callback.
3. **Synchronization & Launch:** The BSP calls ``smp_launch_all()``,
   setting ``launched = true`` for each online core and sending it a wake
   IPI (``APIC_WAKE_VECTOR``). Woken APs exit their wait loop and branch
   directly into their assigned ``run_func(core_index)``.


References
==========
- `Intel SDM Vol. 3
  <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html>`_
  — §9.4.4 (MP Initialization Protocol), §11.6.1 (INIT/SIPI IPIs).
- `AMD64 Architecture Programmer's Manual Vol. 2
  <https://www.amd.com/en/search/documentation/hub.html>`_
  — §16.5 (Inter-Processor Interrupts).


Related Files
=============
- ``kernel/smp.c``
- ``include/smp.h``
- ``boot/ap_trampoline.asm``
- ``boot/multiboot2.asm``

