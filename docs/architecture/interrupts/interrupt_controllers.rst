=====================
Interrupt Controllers
=====================

Overview
========
eMMK manages hardware interrupts using two different controller
architectures: the legacy 8259 PIC and the modern APIC (Advanced
Programmable Interrupt Controller).

During the boot process, the kernel briefly initializes and remaps the PIC
to ensure a stable environment, then disables it entirely in favor of
the APIC. The APIC is required for symmetric multiprocessing (SMP) and more
flexible interrupt routing.


The Legacy 8259 PIC
===================
The PIC consists of two cascaded chips (Primary and Secondary), providing
15 usable hardware interrupt lines (IRQ 0–15).

Remapping
---------
By default, the BIOS maps IRQ 0–7 to CPU exception vectors (``0x08`` –
``0x0F``). This creates a conflict where a Timer interrupt looks like a
Double Fault. ``pic_init()`` remaps these to the offset ``IRQ_BASE``
(vector 32).

Spurious Interrupts
-------------------
The PIC is prone to "spurious" interrupts on IRQ 7 and IRQ 15. This occurs
if an IRQ line is de-asserted just as the CPU acknowledges it.

.. note::

  The kernel must check ``pic_is_spurious()`` before processing. If IRQ 15
  is spurious, an EOI must be sent to the Primary PIC only. If IRQ 7 is
  spurious, no EOI should be sent at all.

Disabling for APIC
------------------
Before enabling the APIC, the PIC must be masked entirely via
``pic_disable()`` to prevent "ghost" interrupts from reaching the CPU while
the APIC is active.


The APIC Subsystem
==================
The APIC system is split into two distinct components: the Local APIC
(LAPIC) and the I/O APIC.

Local APIC (LAPIC)
------------------
Every CPU core has its own Local APIC. It is responsible for:

- Handling CPU-local interrupts (Timer, Performance Counters, Thermal).
- Sending Inter-Processor Interrupts (IPIs) to other cores.
- Managing the task priority and EOI (End of Interrupt) signals.

The LAPIC is memory-mapped. While its default address is ``0xFEE0_0000``,
eMMK dynamically discovers the base address using the ``IA32_APIC_BASE`` MSR.

I/O APIC
--------
The I/O APIC is the system-wide "switchboard." It receives signals from
hardware devices (like a keyboard or disk) and routes them to a specific
Local APIC (and thus a specific CPU).

The I/O APIC features a Redirection Table with 24 entries. Each entry can
be programmed with:

- **Vector:** The IDT index to trigger.
- **Destination:** The Local APIC ID of the target CPU.
- **Flags:** Trigger mode (Edge / Level) and Polarity (Active High / Low).

APIC Timer
----------
The APIC includes a high-precision timer. Unlike the legacy PIT, the APIC
timer runs at the CPU's bus speed. Because this speed varies between
machines, eMMK performs a calibration sequence during ``lapic_timer_init()``:

1. The legacy PIT (Channel 2) is used as a reference for a fixed duration
   (e.g., 10ms).
2. The kernel measures how many APIC ticks occur during that PIT window.
3. The resulting frequency is used to provide accurate periodic interrupts
   at the requested Hz.


Initialization Flow
===================
To ensure a clean handoff, the kernel follows this strict initialization
order in ``kernel_main()``:

1. ``pic_init()``: Remap legacy IRQs to avoid exception conflicts.
2. ``lapic_init()``: Call ``pic_disable()``; discover and enable the Local
   APIC for the bootstrap processor.
3. ``ioapic_init()``: Map the I/O APIC and mask all redirection entries.
4. ``lapic_timer_init(hz)``: Calibrate and start the system heartbeat.


.. list-table:: **APIC Vector Map**
   :header-rows: 1
   :widths: 20 40

   * - Vector
     - Usage
   * - **32–47**
     - Reserved for I/O APIC (mapped IRQs).
   * - **48**
     - APIC Timer.
   * - **49**
     - APIC Error Interrupt.
   * - **255**
     - Spurious Interrupt Vector.


References
==========
- `Intel SDM Vol. 3
  <https://software.intel.com/content/www/us/en/develop/articles/intel-sdm.html>`_
  — Advanced Programmable Interrupt Controller (§11).
- `OSDev Wiki: 8259 PIC
  <https://wiki.osdev.org/8259_PIC>`_
  — detailed remapping and spurious IRQ handling.
- `OSDev Wiki: APIC Timer
  <https://wiki.osdev.org/APIC_Timer>`_
  — calibration techniques.


Related Files
=============
- ``include/pic.h``
- ``include/apic.h``
- ``kernel/pic.c``
- ``kernel/apic.c``

