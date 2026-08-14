===============
Timer Subsystem
===============

Overview
========
The timer subsystem provides eMMK with a monotonic time source and
high-precision delay utilities. It is driven by the Local APIC timer,
which triggers periodic interrupts to increment a global tick counter.

By default, the kernel operates at a frequency of 1000 Hz (defined by
``TIMER_HZ``), providing a 1-millisecond resolution for system ticks.

Operation
=========
The subsystem utilizes two distinct hardware mechanisms to provide time
services:

- **System Ticks (APIC Timer):** Used for millisecond-precision tracking
  and long-term uptime. Every interrupt increments a 64-bit counter.
- **Microsecond Delays (TSC):** The Time Stamp Counter (TSC) is used for
  sub-millisecond busy-waits (``timer_delay_us``). Because the TSC frequency
  matches the CPU clock, it is calibrated against the APIC timer during
  initialization to ensure accuracy across different hardware.


Tick vs. Time
=============
While the kernel tracks time in "ticks," several helper functions are
provided to convert these into human-readable units. At 1000 Hz, one tick
is exactly 1 ms.

.. list-table:: **Time Conversion API**
   :header-rows: 1
   :widths: 30 45

   * - Function
     - Description
   * - ``timer_get_ms()``
     - Total milliseconds elapsed since boot.
   * - ``timer_get_uptime()``
     - Total seconds elapsed since boot (truncated).
   * - ``timer_ms_to_ticks(ms)``
     - Converts a duration in ms to the equivalent number of kernel ticks.


Blocking vs. Polling
====================
As eMMK is currently a run-to-completion kernel, there is no "thread
sleeping" in the traditional sense.

.. warning::

  Calling ``timer_sleep_ms()`` or ``timer_sleep_ticks()`` results in a
  busy-wait loop. This blocks the current CPU core entirely, preventing
  any other processing from occurring until the duration expires.

Usage during Initialization
---------------------------
Busy-waits are acceptable during early boot (e.g., waiting for hardware to
settle after a reset).

.. code-block:: c

  // Wait 50ms for a device to reset
  timer_sleep_ms(50);

Deadline-Based Polling
----------------------

For performance-critical code or device drivers, deadline-based polling is
the preferred pattern. This allows the loop to check for device readiness
while maintaining a fail-safe timeout.

.. code-block:: c

  // Wait up to 100ms for a status bit
  uint64_t deadline = timer_deadline_ms(100);

  while (!(inb(STATUS_PORT) & READY_BIT))
  {
      if (timer_deadline_expired(deadline))
      {
          kprintf("Error: Device timed out!\n");
          return -1;
      }
      cpu_relax();
  }


Precision
=========
The ``timer_delay_us()`` function provides ultra-short delays. Unlike the
millisecond functions, this uses the CPU's TSC (Time Stamp Counter).

- **Interrupt Independence:** This function works even when interrupts are
  disabled (e.g., inside a critical section or an ISR).
- **Calibration:** Requires ``timer_init()`` to have successfully calibrated
  the TSC frequency.


Initialization Requirements
===========================
The timer must be initialized early in the boot process, but it has
specific dependencies:

- **IDT:** Must be loaded so the APIC timer interrupt can be caught.
- **APIC:** The Local APIC must be enabled and mapped.
- **Interrupts:** The CPU must have interrupts enabled (``STI``) during
  ``timer_init()`` because the TSC calibration routine waits for APIC
  timer ticks to occur.


References
==========
- `OSDev Wiki: APIC Timer
  <https://wiki.osdev.org/APIC_Timer>`_
  — configuring the Local APIC for periodic interrupts.


Related Files
=============
- ``include/timer.h``
- ``kernel/timer.c``
- ``include/tsc.h``
- ``kernel/tsc.c``

