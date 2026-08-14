===========================
String and Memory Utilities
===========================

Overview
========
eMMK provides a freestanding, zero-dependency implementation of the
standard C string library (``string.h``). Because the kernel does not link
against a hosted C standard library, these functions provide the essential
primitives for memory manipulation and string parsing used by the VMM,
and drivers.

**Design Principles:**

- **Freestanding:** No dependencies on external libraries or OS features.
- **Safety First:** Preference for bounded APIs (``strlcpy``) over
   legacy C functions.
- **Predictability:** Strictly follows POSIX/ISO C semantics unless
  otherwise noted.


Memory Operations
=================
Memory functions operate on raw byte buffers. These are often optimized by
the compiler into specialized instructions (like ``rep stosb`` or ``rep movsq``)
depending on the architecture and optimization level.

.. list-table:: **Memory Primitives**
   :header-rows: 1
   :widths: 25 75

   * - Function
     - Description
   * - ``memset``
     - Fills a block of memory with a specific byte value.
   * - ``memzero``
     - Helper macro/inline for ``memset(ptr, 0, n)``. Preferred for
       clearing structures.
   * - ``memcpy``
     - Copies memory from ``source`` to ``destination``. Undefined if
       the regions overlap.
   * - ``memmove``
     - Copies memory safely even if the ``source`` and ``destination``
       regions overlap.
   * - ``memcmp``
     - Lexicographically compares two memory blocks.
   * - ``memchr``
     - Locates the first occurrence of a byte in a memory block.

.. note::
   Use ``memcpy`` for performance when regions are guaranteed to be distinct.
   Use ``memmove`` when shifting data within the same buffer (e.g., moving
   entries in an array).


String Examination
==================
These functions are used to inspect null-terminated strings. Prefer
the n-variant (e.g., ``strnlen``) when dealing with untrusted data or
fixed-size buffers.

.. list-table:: **Examination Functions**
   :header-rows: 1
   :widths: 20 80

   * - Function
     - Description
   * - ``strlen``
     - Returns the length of a string (excluding null terminator).
   * - ``strnlen``
     - Returns the length up to a specified maximum; prevents OOB reads
       on non-terminated strings.
   * - ``strcmp``
     - Compares two strings.
   * - ``strncmp``
     - Compares two strings up to ``n`` characters.
   * - ``strchr / strrchr``
     - Find the first or last occurrence of a character.
   * - ``strstr``
     - Finds the first occurrence of a substring.


Safe String Management
======================
eMMK adopts the BSD-style ``strlcpy`` and ``strlcat`` interfaces. Unlike
the standard ``strncpy``, these functions guarantee null termination and
simplify truncation detection.

.. list-table:: Preferred Copy/Cat APIs
   :header-rows: 1
   :widths: 25 75

   * - Function
     - Core Benefit
   * - strlcpy
     - Always null-terminates; returns ``strlen(src)``.
   * - strlcat
     - Bounded concatenation; always null-terminates if ``size`` > 0.

**Truncation Detection Pattern:**

To check if a string was truncated during a copy, compare the return value
against the buffer size:

.. code-block:: c

   char buf[16];
   size_t len = strlcpy(buf, large_input_string, sizeof(buf));

   if (len >= sizeof(buf))
   {
       // Truncation occurred: handle error or log warning
   }


Legacy and Deprecated APIs
==========================
The following functions are provided only for compatibility with imported
third-party code. Their use in new eMMK kernel code is strictly prohibited
and will trigger compiler warnings.

.. list-table:: **Deprecated Functions**
   :header-rows: 1
   :widths: 20 30 50

   * - Function
     - Replacement
     - Security Hazard
   * - ``strcpy``
     - ``strlcpy``
     - Trivial buffer overflow; no bounds checking.
   * - ``strncpy``
     - ``strlcpy``
     - Does not null-terminate if ``src`` ≥ ``n``.
   * - ``strcat``
     - ``strlcat``
     - Trivial buffer overflow.
   * - ``strncat``
     - ``strlcat``
     - Confusing "size" semantics (size of append vs size of buffer).


References
==========
- `ISO/IEC 9899:2024 (C23)`
  - §7.26 (``<string.h>``).
- `OpenBSD Manual
  <https://man.openbsd.org/strlcat.3>`_
  - ``strlcpy``, ``strlcat``.
- `OSDev Wiki: C Library
  <https://wiki.osdev.org/C_Library>`_
  - general guidance on implementing freestanding string functions.


Related Files
=============
- ``include/string.h``
- ``kernel/lib/string.c``
- ``include/types.h``

