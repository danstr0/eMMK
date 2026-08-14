==========================
Endianness & Byte Swapping
==========================

Overview
========
eMMK runs on the x86_64 architecture, which is Little-Endian. However, most
network protocols (TCP/IP) use Big-Endian (also known as network-byte-order).


Implementation
==============
To ensure maximum performance, eMMK uses GCC/Clang built-in intrinsics:

- ``__builtin_bswap16``
- ``__builtin_bswap32``
- ``__builtin_bswap64``

On x86, these built-ins compile directly to a single ``BSWAP`` instruction
(or a ``ROL`` for 16-bit values), meaning the conversion cost is nearly zero.

API Reference
=============

Standard POSIX Aliases
----------------------
These are the primary macros used when writing network code:

.. list-table::
   :widths: 30 50

   * - **Macro**
     - **Meaning**
   * - ``htons(val)`` / ``ntohs(val)``
     - Host ↔ Network Short (16-bit).
   * - ``htonl(val)`` / ``ntohl(val)``
     - Host ↔ Network Long (32-bit).
   * - ``htonll(val)`` / ``ntohll(val)``
     - Host ↔ Network Long Long (64-bit).


Explicit Conversions
--------------------
For non-network code, these macros provide more explicit naming.

- **To big-endian:** ``cpu_to_be16``, ``cpu_to_be32``, ``cpu_to_be64``.
- **From big-endian:** ``be16_to_cpu``, ``be32_to_cpu``, ``be64_to_cpu``.
- **To / from little-endian:** ``cpu_to_le*`` and ``le*_to_cpu``.

.. note::

  Since eMMK is x86-only, the little-endian macros are "identity"
  macros—they do nothing and return the value as-is. They are provided to
  make the code's intent clear when dealing with LE hardware structures.

Usage Example
=============
When filling out a ``netbuf`` for an Ethernet frame, the ``ethertype``
must be in big-endian:

.. code-block:: c

  struct eth_hdr *eth = netbuf_push(nb, sizeof(struct eth_hdr));
  // 0x0800 (IP) must be swapped in memory
  eth->type = htons(ETH_P_IP); 


Related Files
=============
- `include/endian.h`

