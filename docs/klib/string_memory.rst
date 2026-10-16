===========================
String and Memory Utilities
===========================

Overview
========
eMMK provides a freestanding, zero-dependency implementation of the
standard C string library (``string.h``). Because the kernel does not link
against a hosted C standard library, these functions provide the essential
primitives for memory manipulation and string parsing used throughout
the kernel.

**Design Principles:**

- **Freestanding:** No dependencies on external libraries or OS features.
- **Safety First:** Preference for bounded APIs (``strlcpy``) over
   legacy C functions.
- **Predictability:** Strictly follows POSIX/ISO C semantics unless
  otherwise noted.


Memory Operations
=================
Memory functions operate on raw byte buffers.

Implementation
--------------
- **Builtin routing.** ``-ffreestanding`` implies ``-fno-builtin``, which
  would make every ``memcpy`` — even a 6-byte MAC copy — an out-of-line
  call. ``string.h`` therefore maps ``memset``, ``memcpy``, ``memmove``,
  and ``memcmp`` to ``__builtin_*``: GCC expands small constant-size
  operations inline (a few ``mov`` instructions) and calls the real
  function otherwise.
- **Bulk routines.** The out-of-line ``memset`` / ``memcpy`` use
  ``rep stosb`` / ``rep movsb``. On CPUs with ERMSB these are the fastest
  bulk routines available without vector registors (which the kernel
  cannot yet use).
- **No self-recursion.** The build passes
  ``-fno-tree-loop-distribute-patterns`` so GCC never rewrites a loop
  inside ``string.c`` into a call to the very function being defined.

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


Intentionally Omitted APIs
==========================
The following standard functions are **not provided**. Code that calls
them fails to link; use the replacement instead.

.. list-table:: **Omitted Functions**
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


Formatted Output into Buffers
=============================
``ksnprintf`` / ``kvsnprintf`` (``include/format.h``) format into a
fixed-size buffer with ``snprintf`` semantics: always null-terminated,
and the return value is the untruncated length. They share the formatting
engine with ``kprintf``; see :doc:`console_logging`.


References
==========
- `ISO/IEC 9899:2018 (C17)`
  - §7.24 (``<string.h>``).
- `OpenBSD Manual
  <https://man.openbsd.org/strlcat.3>`_
  - ``strlcpy``, ``strlcat``.
- `OSDev Wiki: C Library
  <https://wiki.osdev.org/C_Library>`_
  - general guidance on implementing freestanding string functions.


Related Files
=============
- ``include/string.h``
- ``kernel/string.c``
- ``include/format.h``
- ``kernel/format.c``
- ``test/test_string.c``

