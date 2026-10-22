================================
Symmetric Multi-Processing (SMP)
================================

Overview
========

eMMK initializes Application Processors (APs) after the Bootstrap Processor
(BSP) completes basic subsystem setup (LAPIC, IDT, PMM/VMM, and timers).
APs are discovered, brought out of real mode via an INIT-SIPI-SIPI sequence,
guided through long-mode activation by a low-memory assembly trampoline,
and transitioned into a per-core run loop in C.

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
     │  2. alloc_ap_stack()   [allocate 16 KiB per AP]
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
     │                                ├── Reload segment selectors
     │                                ├── Load IDT (lidt)
     │                                ├── Enable LAPIC & TPR
     │                                ├── Init TSS & Per-CPU (GS base)
     │                                ├── Enable Interrupts (sti)
     │                                ├── Signal online & ap_ready=1
     │                                └── HLT loop (await launch)
     ▼
  smp_launch_all()
     │
     └── Set cores[i].launched = true ───────────────▶ Executes assigned run_func()


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
     - Virtual address of per-AP stack top (16 KiB allocated).
   * - ``+0x20``
     - ``ap_ready``
     - Set to ``1`` by AP when C environment is active.
   * - ``+0x24``
     - ``ap_id``
     - Target APIC ID assigned by BSP.


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
     - Load ``RSP``, setup LAPIC/IDT/TSS, set ``GS_BASE`` via MSR, issue
       ``sti``.


Per-Core Identification & Storage
=================================
Core numbering follows a dense index scheme ``0 .. smp_get_core_count()-1``,
in which Core 0 is strictly reserved for the BSP.

Fast-Path Core Identification
-----------------------------
When querying the current core index via ``smp_core_id()``:

1. **Fast path:** Reads ``MSR_GS_BASE`` (``0xC0000101``). If initialized
   (non-zero), it returns ``this_core_id()`` directly from the per-CPU
   data structure via a single instruction.
2. **Early boot fallback:** If ``GS_BASE`` is zero (pre-percpu setup), it
   reads the Local APIC ID directly from the LAPIC hardware register and
   performs a lookup against the global ``cores[]`` array.


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

