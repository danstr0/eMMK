=========================
Interrupts and Exceptions
=========================

Overview
========
eMMK implements a full 64-bit Interrupt Descriptor Table (IDT) to handle
CPU exceptions, hardware interrupts (IRQs), and system calls. The system
bridges the gap between low-level hardware transitions and high-level C
logic using a series of assembly "stubs" that normalize the stack state
before dispatching to C.


IDT Layout
==========
The x86-64 IDT consists of 256 entries. Each entry is a 16-byte gate
descriptor pointing to an Interrupt Service Routine (ISR).

.. list-table:: **Vector Assignments**
   :header-rows: 1
   :widths: 20 60

   * - Vectors
     - Description
   * - **0–31**
     - CPU Exceptions. Internal events like Page Faults (``#PF``) or
       Divide Errors (``#DE``).
   * - **32–47**
     - Hardware IRQs. Remapped from the legacy PIC (Programmable Interrupt
       Controller).
   * - **48–255**
     - User-Defined. Reserved for APIC interrupts, MSIs, and software
       interrupts.


Gate Descriptors
================
Each gate in the IDT is configured with specific attributes to control
privilege and behavior:

.. list-table:: **Gate Attributes**
   :widths: 25 50

   * - **Type**
     - Interrupt Gate (clears IF) or Trap Gate (leaves IF unchanged).
   * - **DPL**
     - Descriptor Privilege Level. Usually 0 (Kernel) or 3 (User/System Call).
   * - **IST**
     - Interrupt Stack Table index. Allows switching to a known-good stack
       (crucial for Double Faults).
   * - **Target**
     - A 64-bit virtual address pointing to the assembly stub in ``isr.asm``.


Interrupt Flow
==============
When an interrupt occurs, the transition from hardware to C code follows
a strict sequence to ensure the register state is preserved and the stack
is aligned.

1. **Hardware Save:** The CPU pushes ``SS``, ``RSP``, ``RFLAGS``, ``CS``,
   and ``RIP`` onto the stack. If the exception provides one, an Error
   Code is also pushed.
2. **ISR Stub:** The stub pushes a dummy error code (if missing) and the
   Vector Number to create a uniform stack frame.
3. **State Preservation:** All 15 general-purpose registers (``RAX`` - ``R15``)
   are pushed.
4. **C Dispatch:** The stub calls ``interrupt_dispatch``, passing the current
   ``RSP`` as a pointer to a struct ``interrupt_frame``.
5. **Restoration:** Once the C handler returns, registers are popped, and
   the ``IRETQ`` instruction restores the CPU state.


The Interrupt Frame
===================
The struct ``interrupt_frame`` (defined in ``idt.h``) provides a complete
snapshot of the CPU state at the moment of the interrupt:

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
produces an error code, eMMK uses two macros in isr.asm to generate
the 256 required stubs:

- ``ISR_NOERR``: Used for interrupts that don't provide an error code. It
  pushes a "dummy" 0 to keep the stack consistent.
- ``ISR_ERR``: Used for exceptions like Page Faults that already have an
  error code on the stack.

All stubs eventually converge at ``isr_common``, which handles the transition
into the C ABI (including 16-byte stack alignment).


API Reference
=============

.. list-table::
   :widths: 30 45

   * - ``idt_init()``
     - Initializes the IDT, loads stubs, and executes ``LIDT``.
   * - ``idt_set_handler(vec, fn)``
     - Registers a C function to be called when a specific vector triggers.
   * - ``IRQ_VECTOR(irq)``
     - Macro to map a legacy IRQ (0-15) to its IDT index (32-47).

Example: Registering a Keyboard Handler
---------------------------------------

.. code-block:: c

  void keyboard_handler(struct interrupt_frame *frame)
  {
      uint8_t scancode = inb(0x60);
      // Process key...
  }

  // Inside driver initialization:
  idt_set_handler(IRQ_VECTOR(IRQ_KEYBOARD), keyboard_handler);


References
==========
- `Intel SDM Vol. 3
  <https://software.intel.com/content/www/us/en/develop/articles/intel-sdm.html>`_
  — Interrupt and Exception Handling (§6).

- `OSDev Wiki: Interrupt Descriptor Table
  <https://wiki.osdev.org/Interrupt_Descriptor_Table>`_
  — x86-64 gate formats and requirements.


Related Files
=============
- ``include/idt.h``
- ``kernel/isr.asm``
- ``kernel/idt.c``

