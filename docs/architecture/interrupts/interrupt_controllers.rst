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

Register Access
---------------
The LAPIC and IOAPIC register pages are mapped strong-uncacheable through
``vmm_map_mmio()``. Every access therefore reaches the device in program
order, and ``lapic_write()`` needs no serializing read-back.

APIC Timer
----------
The APIC includes a timer that runs at the bus clock, which varies between
machines. ``lapic_timer_init(hz)`` calibrates it against PIT channel 2 (a
10 ms one-shot; see ``include/pit.h``) and starts periodic interrupts on
the calling core.

eMMK is tickless, so **nothing starts the APIC timer at boot**; time is
kept by the TSC (doc:`timer`). The driver remains for code that wants a
periodic interrupt, and the test suite uses it as an independent clock.

Inter-Processor Interrupts
--------------------------
- ``lapic_send_ipi(apic_id, vector)`` / ``lapic_send_ipi_all(vector)``:
  fixed-delivery IPIs. The handler msut send an EOI.
- ``APIC_WAKE_VECTOR`` (``0xF0``): brings a core out of ``hlt``; used to
  launch the APs.
- ``lapic_send_nmi_all()``: NMI to all other cores; used only by the panic
  path to stop them.


Initialization Flow
===================
The kernel follows this order in ``kernel_main()``:

1. ``pic_init()``: Remap legacy IRQs to avoid exception conflicts.
2. ``lapic_init()``: Mask the PIC (``pic_disable()``); discover, map, and
   enable the Local APIC; register the error, spurious, and wake-up
   handlers.
3. ``ioapic_init()``: Map the I/O APIC and mask all redirection entries.

No timer is started. See :doc:`interrupts` for the complete vector map.


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

