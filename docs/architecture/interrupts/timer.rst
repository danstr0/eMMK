==================
Time and the Clock
==================

Overview
========
eMMK keeps time with the **TSC** (Time Stamp Counter) and nothing else.
There is no periodic timer interrupt: the kernel is *tickless*. Reading
the clock is one ``rdtsc`` and a multiply, works with interrupts disabled,
and never disturbs another core.

.. list-table::
   :header-rows: 1
   :widths: 25 60

   * - Layer
     - Provides
   * - ``tsc.h``
     - Raw ``tsc_read()``, calibration, division-free cycle → ns / µs /
       ms conversion, ``tsc_delay_us()``
   * - ``timer.h``
     - Monotonic time since boot (``timer_get_ns/us/ms``), a 1 ms tick
       counter, busy-wait sleeps, deadline helpers
   * - ``apic.h``
     - Optional periodic LAPIC timer interrupts


Why Tickless?
=============
The kernel used to run the LAPIC timer at 1000 Hz on the BSP to keep a
tick counter. That had three costs:

- **Jitter on the data path.** In the demo the BSP is the distributor:
  every millisecond its RX polling was interrupted.
- **Interrupt dependence.** Sleeps and timeouts only advanced with
  interrupts enabled, so ``timer_sleep_*`` and ``tsc_init`` silently
  executed ``sti``.
- **Lost time.** A section that kept interrupts disabled for longer than
  1 ms (e.g., printing a long line over a real UART) lost ticks, so uptime
  drifted.

The TSC has none of these problems.


Calibration
===========
``tsc_init()`` determines the TSC frequency once, without interrupts:

1. **CPUID leaf 0x15**, if it reports a complete TSC/crystal ratio and
   crystal frequency (common on recent Intel CPUs).
2. Otherwise the **PIT**: channel 2 counts down 50 ms in one-shot mode
   (polled through port ``0x61``, interrupts disabled) while the TSC is
   read at either end. Error is a few microseconds over 50 ms.

``tsc_init()`` also records the boot TSC value (time zero) and checks
CPUID for an **invariant TSC** (constant rate in all power states).


Division-Free Conversion
========================
Conversions use fixed-point factors computed at calibration:

.. code-block:: c

   ns = (cycles * tsc_ns_mult) >> 32;
   us = (cycles * tsc_us_mult) >> 40;
   ms = (cycles * tsc_ms_mult) >> 48;

One 64×64→128-bit multiply and a shift (a few cycles), instead of a 64-bit
division (tens of cycles).


Time API
========

.. list-table::
   :header-rows: 1
   :widths: 35 50

   * - Function
     - Description
   * - ``timer_get_ns()`` / ``_us()`` / ``_ms()``
     - Time since ``tsc_init()``.
   * - ``timer_get_ticks()``
     - Milliseconds (``TIMER_HZ`` = 1000); kept for coarse timestamps
       such as ARP cache ages.
   * - ``timer_sleep_ms(ms)``
     - Busy-wait the full duration (cycle resolution).
   * - ``timer_deadline_ms(ms)`` / ``timer_deadline_expired(d)``
     - Non-blocking timeouts for poll loops.
   * - ``tsc_delay_us(us)``
     - Microsecond busy-wait.

All of them work with interrupts disabled and leave the interrupt flag
untouched.

.. warning::

   Sleeps are busy-waits: the core does nothing else for the duration.
   This is fine during initialization, but on a data path use deadlines.

Deadline-Based Polling
----------------------

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


Measuring Latency
=================

.. code-block:: c

   uint64_t t0 = tsc_read();
   process(pkt);
   uint64_t t1 = tsc_read();
   uint64_t ns = tsc_elapsed_ns(t0, t1);

``tsc_read()`` issues ``lfence`` before ``rdtsc``, so the read cannot
execute before earlier instructions complete.


The LAPIC Timer
===============
``lapic_timer_init(hz)`` still starts periodic interrupts on the calling
core (calibrated against the PIT), counted by ``lapic_timer_get_ticks()``;
``lapic_timer_stop()`` stops them. Nothing does this at boot. The test
suite uses it to cross-check the TSC against an independent clock.


Initialization Requirements
===========================
- ``tsc_init()`` needs nothing but the PIT; it runs after the APIC setup
  in ``kernel_main()`` and before anything that uses time.
- ``timer_init()`` must follow ``tsc_init()``.
- Before ``tsc_init()``, the clock reads 0 and delays return immediately.


References
==========
- `Intel SDM Vol. 3
  <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html>`_
  — Time-Stamp Counter (invariant TSC, CPUID leaf 15H).
- `OSDev Wiki: PIT <https://wiki.osdev.org/Programmable_Interval_Timer>`_
  — channel 2 and port 0x61.


Related Files
=============
- ``include/tsc.h`` / ``kernel/tsc.c``
- ``include/timer.h`` / ``kernel/timer.c``
- ``include/pit.h``
- ``test/test_interrupts.c``

