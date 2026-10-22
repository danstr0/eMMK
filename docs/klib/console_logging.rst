=====================
Console and Logging
=====================

Overview
========
All kernel output goes to the first serial port (COM1, ``0x3F8``) in
polled mode. Three layers sit on top of the UART:

.. list-table::
   :header-rows: 1
   :widths: 25 25 50

   * - Layer
     - API
     - Role
   * - UART driver
     - ``serial_init``, ``serial_putchar``
     - 8N1 at 115200 baud, FIFOs on, interrupts off. ``serial_putchar``
       spins on the line-status register, then writes one byte.
   * - Console
     - ``kprintf``, ``kputs``, ``kputchar``
     - Locked, ``\n`` → ``\r\n`` translated output on COM1.
       ``kputchar`` is unlocked and untranslated.
   * - Leveled log
     - ``klog``, ``klog_info`` …
     - Adds a ``[core:tag]`` prefix and a runtime severity filter.


Formatting
==========
``kprintf`` and ``ksnprintf`` share one engine, ``kvformat``
(``kernel/format.c``), which writes characters to a sink callback.

.. list-table::
   :header-rows: 1
   :widths: 25 60

   * - Feature
     - Supported
   * - Conversions
     - ``d i u x X o p s c %``
   * - Flags
     - ``-`` ``0`` ``+`` space ``#``
   * - Width / precision
     - digits or ``*``; precision is minimum digits for integers and
       maximum characters for ``%s``
   * - Length modifiers
     - ``hh h l ll z j t``

Every specifier GCC's ``-Wformat`` accepts for these conversions
consumes exactly its argument, so a format string cannot desynchronize
the argument list. Floating-point conversions are not supported; an
unknown conversion is printed literally.

Use ``uint64_t`` with ``%lu`` / ``%lx`` (it is ``unsigned long`` on
LP64) and ``size_t`` with ``%zu``.


Locking and Panic Mode
======================
``kprintf`` / ``kputs`` take a global spinlock with interrupts disabled
for the entire message, so lines from different cores never interleave.

A fatal exception handler must still be able to print — even when the
fault happened *inside* ``kprintf`` while this core held the lock (a bad
``%s`` pointer, for example), where taking the lock again would spin
forever. The default exception handler therefore calls
``serial_panic_enter()`` first. Panic mode is one-way: from then on
console output skips the lock, accepting that other cores' output may
interleave with the panic report.


Latency Hazards
===============
Console output is **synchronous and slow**:

- At 115200 baud the UART drains ~11.5 characters per millisecond, so a
  60-character line takes ~5 ms on real hardware. The calling core
  spins for that entire time **with interrupts disabled**, and every
  other core that tries to print spins on the lock.
- Interrupts are held off for the whole message.
- Under KVM every ``in`` / ``out`` instruction is a VM exit; each
  character costs at least two (status poll + data write).
- QEMU's emulated UART is effectively instantaneous, which hides all of
  the above.

Consequently: **never print from a data-path core.** The demo's
periodic benchmark report runs on the distributor and stalls RX polling
while it prints.

.. note::

   Planned: per-core lock-free log rings that data-path cores append to
   in nanoseconds, drained to the UART by a non-critical core.


Leveled Logging (``klog``)
==========================

.. code-block:: c

   klog(LOG_INFO, "ETH", "frame received, len=%u\n", len);
   // → [2:ETH] frame received, len=64

   log_set_level(LOG_WARN);   // silences INFO / DEBUG

Levels: ``LOG_NONE`` (0), ``LOG_ERROR``, ``LOG_WARN``, ``LOG_INFO``
(default), ``LOG_DEBUG`` (4). A suppressed message costs one compare
and a predictable branch; an enabled one costs a full ``kprintf``
(see above). ``klog`` reads the core index via ``smp_core_id()``, which
falls back to the LAPIC ID before per-CPU data exists — so ``klog`` is
usable once the LAPIC is initialized; before that, use ``kprintf``.


Related Files
=============
- ``include/serial.h`` / ``kernel/serial.c``
- ``include/format.h`` / ``kernel/format.c``
- ``include/log.h`` / ``kernel/log.c``
- ``test/test_string.c`` (formatter tests)

