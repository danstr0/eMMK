=========================
Interrupts and Exceptions
=========================

Overview
========
eMMK implements a full 64-bit Interrupt Descriptor Table (IDT) for CPU
exceptions, device interrupts, and inter-processor interrupts (IPIs).
Assembly stubs normalize the stack state and dispatch to C handlers.

Interupts play a deliberately small role: the data path is polled, the
kernel is tickless (no periodic timer), and interrupts exist only to
wake idle cores, to report faults, and to stop the machine on a panic.


Interrupt Policy
================
- **Data-path cores run with interrupts disabled.** Nothing interrupts
  them, so nothing adds jitter.
- **No periodic timer.** Time comes from the TSC (see :doc:`timer`).
  The APIC timer driver exists but is not started at boot.
- **Device interrupts only wake.** A device interrupt handler acknowledges
  the interrupt and returns. All packet processing happens in poll loops.
- **Idle loops use** ``sti; hlt``. Poll with interrupts disabled, then
  ``sti; hlt``: ``sti`` takes effect only after the following instruction,
  so an interrupt that arrives after the last poll wakes the ``hlt``
  instead of being taken just before it and lost.
- **NMIs mean panic.** eMMK sends NMIs only to stop cores during a panic;
  an NMI at any other time is treated as a fatal error.


Vector Map
==========

.. list-table::
   :widths: 15 25 60

   * - Vector
     - Name
     - Use
   * - 0–31
     - CPU exceptions
     - Unhandled ones panic. 2 = NMI (panic broadcast); 8 (#DF) and 2
       (NMI) run on dedicated IST stacks.
   * - 32–47
     - Legacy PIC range
     - The PIC is remapped here and fully masked. Also used for the
       IOAPIC INTx fallback (``IRQ_VECTOR(n)``).
   * - 48
     - ``APIC_TIMER_VECTOR``
     - Only if ``lapic_timer_init()`` is called.
   * - 49
     - ``APIC_ERROR_VECTOR``
     - Logs the APIC error status.
   * - 50
     - ``VIRTIO_NET_MSIX_VECTOR``
     - virtio-net wake-up (acknowledge only).
   * - 0xF0
     - ``APIC_WAKE_VECTOR``
     - Wake-up IPI: brings a core out of ``hlt``; the handler only EOIs.
   * - 0xFF
     - ``APIC_SPURIOUS_VECTOR``
     - APIC spurious interrupts; no EOI. **NEVER send an IPI here.**

.. warning::

   A *fixed* IPI on the spurious vector sets its in service bit, and the
   spurious handler (correctly, for real spurious interrupts) sends no
   EOI. The vector then stays in service and blocks every interrupt of
   equal or lower priority on the target core — i.e., all of them.


Gate Descriptors
================
Each gate in the IDT is configured with specific attributes to control
privilege and behavior:

.. list-table:: **Gate Attributes**
   :widths: 25 50

   * - **Type**
     - Interrupt gate (clears IF on entry). All gates use this type.
   * - **DPL**
     - 0 (kernel only; eMMK has no user mode).
   * - **IST**
     - Interrupt Stack Table index. #DF uses IST1 and NMI uses IST2
       (``idt_configure_ist()``), so they run on a known-good stack even
       after a kernel stack overflow.
   * - **Target**
     - The vector's assembly stub in ``isr.asm``.


Interrupt Flow
==============

1. **Hardware Save:** The CPU aligns RSP to 16 bytes, then pushes ``SS``,
   ``RSP``, ``RFLAGS``, ``CS``, and ``RIP`` (and an error code for some
   exceptions).
2. **ISR Stub:** The stub pushes a dummy error code (if needed) and the
   vector number, to create a uniform frame.
3. **Common entry:** ``isr_common`` clears the direction flag (``cld``:
   the C ABI requires DF=0, and compiler-generated ``rep movs`` relies on
   it), then pushes all 15 general-purpose registers.
4. **C Dispatch:** It calls ``interrupt_dispatch(frame)``, which invokes
   the registered handler, panics for unhandled exceptions, or logs and
   EOIs anything else.
5. **Restoration:** Registers are popped, the vector and error code
   discarded, and ``IRETQ`` restores the interrupted context (including
   its RFLAGS).

FPU/SSE state is **not** saved: the kernel is compiled without vector
instructions.


The Interrupt Frame
===================

.. code-block:: c

  struct interrupt_frame
  {
      uint64_t r15, r14, ..., rax;  // general purpose registers
      uint64_t vector;              // the interrupt number (0-255)
      uint64_t error_code;          // CPU error code or 0
      uint64_t rip, cs, rflags...;  // pushed by hardware
  };


ISR Stubs
=========
Because the CPU behaves differently depending on whether an exception
produces an error code, ``isr.asm`` uses two macros to generate the 256
stubs:

- ``ISR_NOERR``: pushes a dummy 0 to keep the stack consistent.
- ``ISR_ERR``: for exceptions (#DF, #TS, #NP, #SS, #GP, #PF, #AC, #CP)
  that already pushed an error code.


Kernel Panic
============
``panic(fmt, ...)`` (software) and ``panic_exception(frame)`` (unhandled
exceptions) stop the **whole machine**, not just the faulting core —
a crashed worker would otherwise leave the rest of the pipeline running
on corrupted state.

1. The first panicking core claims ownership with an atomic
   compare-exchange; any other core that panics concurrently halts
   silently, as does a nested fault on the owner.
2. The console switches to lock-free panic mode, so the report cannot
   deadlock on a console lock the faulting code may hold.
3. An NMI is broadcast to all other cores (once the LAPIC is up). Their
   NMI handler sees the panic and halts. NMIs are delivered even to cores
   spinning with interrupts disabled.
4. The owner prints its report — for exceptions: name, error code, all
   registers, and page-fault details — and halts.

The fatal-error paths in the PMM, VMM, and slab allocator use ``panic()``.


API Reference
=============

.. list-table::
   :widths: 30 45

   * - ``idt_init()``
     - Builds the IDT, installs the panic NMI handler, executes ``LIDT``.
   * - ``idt_configure_ist()``
     - Moves #DF and NMI onto IST stacks (after ``tss_init()``).
   * - ``idt_set_handler(vec, fn)``
     - Registers a C handler (``NULL`` to unregister). The table is shared
       by all cores.
   * - ``IRQ_VECTOR(irq)``
     - Maps a legacy IRQ (0-15) to its IDT index (32-47).

Example: a wake-only device interrupt
-------------------------------------

.. code-block:: c

  static void nic_irq_handler(struct interrupt_frame *frame)
  {
      uint8_t scancode = inb(0x60);
      (void)frame;
      // Acknowledge at the device if it requires it, then the LAPIC.
      // Do NOT process packets here: the poll loop does that.
      lapic_eoi();
  }

  idt_set_handler(NIC_MSIX_VECTOR, nic_irq_handler);


References
==========
- `Intel SDM Vol. 3
  <https://software.intel.com/content/www/us/en/develop/articles/intel-sdm.html>`_
  — Interrupt and Exception Handling; APIC (IPIs, NMI delivery).
- `System V AMD64 ABI <https://gitlab.com/x86-psABIs/x86-64-ABI>`_
  — direction flag on function entry.
- `OSDev Wiki: Interrupt Descriptor Table
  <https://wiki.osdev.org/Interrupt_Descriptor_Table>`_
  — x86-64 gate formats and requirements.


Related Files
=============
- ``include/idt.h``, ``kernel/idt.c``
- ``kernel/isr.asm``
- ``include/panic.h``, ``kernel/panic.c``
- ``test/test_interrupts.c``

