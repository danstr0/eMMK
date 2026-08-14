==============
I/O Primitives
==============

Overview
========
The io.h header provides the absolute lowest layer of hardware abstraction
for the eMMK kernel. These primitives allow the kernel to communicate with
hardware peripherals via the two primary x86 mechanisms: Port-Mapped I/O
(PMIO) and Memory-Mapped I/O (MMIO).

All routines defined here are static inline and include necessary compiler
barriers to ensure that hardware interactions occur in the exact order
specified by the source code.


Port-Mapped I/O (PMIO)
======================
Port I/O uses a separate address space from main memory, accessed via
the x86 ``in`` and ``out`` instructions. These are typically used for legacy
hardware (PIT, PIC, PS/2) and configuration registers (PCI Configuration
Space).

.. list-table:: Scalar Port Operations
   :header-rows: 1
   :widths: 20 20 60

   * - Function
     - Instruction
     - Description
   * - ``inb`` / ``outb``
     - ``AL``
     - 8-bit (Byte) transfer.
   * - ``inw`` / ``outw``
     - ``AX``
     - 16-bit (Word) transfer.
   * - ``inl`` / ``outl``
     - ``EAX``
     - 32-bit (Long/Double Word) transfer.

Example usage
-------------

.. code-block:: c

   // Write a character to the COM1 serial port
   outb(0x3F8, 'A');

   // Read the Master PIC mask register
   uint8_t mask = inb(0x21);


String Port I/O
===============
For bulk data transfers (such as reading disk sectors from an IDE controller
or filling a network FIFO), the kernel uses string-based port primitives.
These map to the ``REP INS`` and ``REP OUTS`` instructions.

.. list-table:: Bulk Transfer Helpers
   :widths: 30 70

   * - ``insl(port, addr, count)``
     - Reads count 32-bit dwords from ``port`` into the buffer at ``addr``.
   * - ``outsl(port, addr, count)``
     - Writes count 32-bit dwords from the buffer at ``addr`` to port``.

.. note::

   String operations assume the destination/source buffer is correctly
   aligned for 32-bit access. The count parameter refers to the number
   of dwords, not the number of bytes.


I/O Delay
=========
Some legacy hardware (notably the 8259A PIC) cannot process back-to-back
I/O commands as fast as a modern CPU can issue them. ``io_wait()`` provides
a predictable, small delay (~1 µs).

It functions by performing a dummy write to port 0x80. On standard PC
hardware, port 0x80 is used for POST (Power-On Self-Test) codes and is
guaranteed to be a safe "no-op" write that forces the CPU to wait for
the I/O bus to cycle.


Memory-Mapped I/O (MMIO)
========================
MMIO maps hardware registers into the standard physical address space.
This allows the kernel to use standard pointer dereferences to interact
with devices like the Local APIC or PCI Express BARs.

.. list-table:: MMIO Accessors
   :header-rows: 1
   :widths: 25 25 50

   * - Data Width
     - Read
     - Write
   * - 8-bit
     - ``mmio_read8``
     - ``mmio_write8``
   * - 16-bit
     - ``mmio_read16``
     - ``mmio_write16``
   * - 32-bit
     - ``mmio_read32``
     - ``mmio_write32``
   * - 64-bit
     - ``mmio_read64``
     - ``mmio_write64``


Safety and Volatility
=====================
MMIO access requires strict adherence to two rules, which these primitives handle automatically:

1. **Volatile Access:** All pointers are cast to ``volatile`` to prevent
   the compiler from caching values in registers or optimizing away
   "redundant" reads/writes.
2. **Software Barriers:** Each function is bracketed by ``barrier()``
   (a compiler memory barrier). This prevents the compiler from reordering
   MMIO accesses across other memory operations.

.. warning::
   These primitives handle **compiler** reordering only. On some
   architectures or complex bus topologies, additional **hardware** memory
   barriers (like``mfence``) may be required to ensure visibility to the
   device.


References
==========
- `Intel SDM Vol. 2
  <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html>`_
  — §3.3 (``IN``, ``INS`` instructions), §4.3 (``OUT``, ``OUTS`` instructions).
- `GCC Inline Assembly
  <https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html>`_
  - asm instructions with C expression operands, §6.11.2.1 (``volatile``),
  §6.11.2.6 (``memory`` clobber).
- `OSDev Wiki: Inline Assembly
  <https://wiki.osdev.org/Inline_Assembly/Examples#IO_WAIT>`_
  - general guidance on inline assembly and the POST code port for I/O delay.


Related Files
=============

- ``include/io.h``
- ``include/types.h``

