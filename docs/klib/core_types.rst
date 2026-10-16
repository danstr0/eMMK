==============================
Core Types and Utility Macros
==============================

Overview
========
``include/types.h`` provides the kernel's foundational type definitions
and utility macros. Since eMMK is built as a freestanding binary
(``-ffreestanding``), the hosted C standard library headers
``<stdint.h>``, ``<stddef.h>``, and ``<stdbool.h>`` are not available
during a kernel build. ``types.h`` reimplements the subset the kernel
needs.

This header is included (directly or transitively) by every kernel
module.


Integer Types
=============

Exact-width types
-----------------

.. list-table::
   :header-rows: 1
   :widths: 25 25 15

   * - Signed
     - Unsigned
     - Width
   * - ``int8_t``
     - ``uint8_t``
     - 8 bits
   * - ``int16_t``
     - ``uint16_t``
     - 16 bits
   * - ``int32_t``
     - ``uint32_t``
     - 32 bits
   * - ``int64_t``
     - ``uint64_t``
     - 64 bits

These are defined in terms of the fundamental C types for the x86-64
LP64 data model (``char`` = 8, ``short`` = 16, ``int`` = 32,
``long`` = 64 bits). ``int64_t`` and ``uint64_t`` are ``long`` /
``unsigned long``, matching GCC's own ``<stdint.h>`` on x86-64, so
``%ld`` / ``%lu`` / ``%lx`` are the correct format specifiers and the
``UL`` literal suffix produces a ``uint64_t``.

Pointer and size types
----------------------

.. list-table::
   :header-rows: 1
   :widths: 20 50

   * - Type
     - Purpose
   * - ``intptr_t``
     - Signed integer wide enough to hold a pointer.
   * - ``uintptr_t``
     - Unsigned integer wide enough to hold a pointer.
   * - ``ptrdiff_t``
     - Signed result of pointer subtraction.
   * - ``size_t``
     - Unsigned size of an object in bytes.
   * - ``ssize_t``
     - Signed counterpart of ``size_t`` (for error returns).

All five are 64-bit on x86-64. Compile-time assertions at the bottom
of the header verify these size assumptions:

.. code-block:: c

   STATIC_ASSERT(sizeof(uintptr_t) == 8, "uintptr_t must be pointer-sized");
   STATIC_ASSERT(sizeof(size_t)    == 8, "size_t must be 8 bytes on x86-64");

Boolean and NULL
----------------

.. list-table::
   :widths: 20 50

   * - ``bool``
     - Alias for ``_Bool`` (C11 keyword).
   * - ``true`` / ``false``
     - Defined as ``1`` / ``0``.
   * - ``NULL``
     - ``((void *)0)``.


Utility Macros
==============

Array size
----------

``ARRAY_SIZE(x)``
   Returns the number of elements in a statically-allocated array.
   Divides the total size of the array by the size of one element.

.. code-block:: c

   int buf[64];
   for (size_t i = 0; i < ARRAY_SIZE(buf); i++)
       buf[i] = 0;

.. warning::

   ``ARRAY_SIZE`` produces a wrong (but silently compiling) result
   when applied to a pointer. Use it only on actual arrays.


Alignment
---------

``ALIGN_UP(x, a)``
   Round *x* up to the next multiple of *a*.

``ALIGN_DOWN(x, a)``
   Round *x* down to the previous multiple of *a*.

**Precondition:** *a* must be a power of two.

Implementation uses the standard bitmask trick:

.. code-block:: c

   #define ALIGN_UP(x, a)    (((x) + ((a) - 1)) & ~((a) - 1))
   #define ALIGN_DOWN(x, a)  ((x) & ~((a) - 1))

These are used extensively in the PMM, VMM, and heap for page and
block alignment.

.. code-block:: c

   ALIGN_UP(0x1001, 0x1000)    // → 0x2000
   ALIGN_DOWN(0x1FFF, 0x1000)  // → 0x1000


Min / max / clamp
-----------------

``MIN(a, b)`` / ``MAX(a, b)``
   Type-safe minimum and maximum.

``CLAMP(val, lo, hi)``
   Restrict *val* to the range [*lo*, *hi*].

All three use GCC statement expressions (``({...})``) to evaluate each
argument exactly once, avoiding the double-evaluation pitfall of
traditional C macros:

.. code-block:: c

   #define MIN(a, b)  ({           \
       __typeof__(a) _a = (a);     \
       __typeof__(b) _b = (b);     \
       _a < _b ? _a : _b;         \
   })

.. note::

   Both operands are independently typed. Mixing signed and unsigned
   types may trigger implicit conversions. The kernel does not
   add a ``_Static_assert`` for type matching (following the same
   convention as the Linux kernel's ``min``/``max``).


Struct Introspection
====================

``offsetof(type, member)``
   Byte offset of *member* within *type*. Implemented via
   ``__builtin_offsetof``.

``container_of(ptr, type, member)``
   Given a pointer to a *member*, recover a pointer to the enclosing
   *type* instance. This is the fundamental primitive for intrusive
   data structures (linked lists, trees, hash tables) where the link
   node is embedded inside the object rather than wrapping it.

.. code-block:: c

   struct task
   {
       int          pid;
       struct list  run_node;   // embedded link
   };

   // Given a pointer to run_node, recover the task:
   struct task *t = container_of(link_ptr, struct task, run_node);


Bit Manipulation
================

.. list-table::
   :widths: 30 50

   * - ``BIT(n)``
     - ``1UL << n`` — a single-bit mask.
   * - ``SET_BIT(x, n)``
     - Set bit *n* in *x*.
   * - ``CLEAR_BIT(x, n)``
     - Clear bit *n* in *x*.
   * - ``TEST_BIT(x, n)``
     - Non-zero if bit *n* is set in *x*.
   * - ``TOGGLE_BIT(x, n)``
     - Flip bit *n* in *x*.

``BIT`` uses ``1UL`` (unsigned 64-bit on LP64) to avoid undefined
behaviour when *n* ≥ 32.


Compiler Attributes
===================
These macros wrap GCC ``__attribute__`` syntax.

.. list-table::
   :widths: 25 50

   * - ``PACKED``
     - Remove inter-field padding. Used on hardware-defined structures
       (page table entries, protocol headers, MMIO registers).
   * - ``ALIGNED(n)``
     - Force *n*-byte alignment. Used on the IDT, page tables, and
       DMA buffers.
   * - ``NORETURN``
     - Function does not return (e.g., ``kernel_main``, panic handlers).
   * - ``UNUSED``
     - Suppress "unused variable/parameter" warnings.
   * - ``LIKELY(x)`` / ``UNLIKELY(x)``
     - Branch prediction hints via ``__builtin_expect``. Used on
       error-checking paths (``UNLIKELY``) and fast paths (``LIKELY``).
   * - ``PRINTF_FMT(fmt, args)``
     - Enable ``-Wformat`` checking on printf-like functions. Applied
       to ``serial_printf`` and ``kprintf``.


Memory Barriers and Volatile Access
===================================
These primitives are essential in a kernel that interacts with
hardware and shared-memory devices (virtio rings, MMIO registers).

``barrier()``
   Compiler memory barrier — prevents the compiler from reordering
   memory accesses across this point. Does **not** emit a CPU fence
   instruction. See *x86 ordering* below for when that is enough.

   .. code-block:: c

      #define barrier()  __asm__ __volatile__("" ::: "memory")

``cpu_relax()``
   Emits the x86 ``PAUSE`` instruction with a compiler memory barrier.
   ``PAUSE`` improves performance and power consumption in spin-wait
   loops by hinting to the CPU that the loop is a busy-wait (avoids
   memory-order violations on speculative loads and reduces power in
   the pipeline).

   .. code-block:: c

      while (!ready)
          cpu_relax();

``READ_ONCE(x)`` / ``WRITE_ONCE(x, val)``
   Perform a single volatile load or store, bracketed by compiler
   barriers. Prevents the compiler from:

   - **Caching** the value in a register across iterations.
   - **Eliding** the access entirely (dead-store elimination).
   - **Splitting** the access into multiple smaller loads/stores.
   - **Reordering** the access relative to adjacent memory operations.

   Used for MMIO registers and any memory location shared with
   hardware or other cores.

x86 ordering
------------
x86-64 uses Total Store Order (TSO) for ordinary (write-back) memory:

- loads are not reordered with other loads;
- stores are not reordered with other stores;
- stores are not reordered with *earlier* loads;
- **a load may be reordered before an earlier store to a different
  address**.

A compiler barrier is sufficient for publish/consume patterns — which
covers spinlock release, SPSC rings, and virtio descriptor → doorbell
sequences. Anything that needs *store then load* ordering across cores
needs a real fence: ``mfence`` or any ``lock``-prefixed instruction.


References
==========
- `ISO/IEC 9899:2011 (C11)
  <https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1570.pdf>`_
  — §6.5.3.4 (``sizeof``), §6.7.2.1 (bitfields), §7.19 (``<stddef.h>``),
  §7.20 (``<stdint.h>``).
- `System V AMD64 ABI
  <https://gitlab.com/x86-psABIs/x86-64-ABI/>`_
  — §3.1.2 (data model: LP64 type sizes).
- `GCC Docs
  <https://gcc.gnu.org/onlinedocs/gcc/>`_
  — §6.4.1 (``packed``, ``aligned``), §6.12.8 (``__builtin_offsetof``), §7.12 (``__builtin_expect``), .
- `Intel SDM Vol. 2
  <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html>`_
  — ``PAUSE`` instruction (§4.3).


Related Files
=============
- ``include/types.h``
- ``include/kernel.h``
- ``include/io.h``
- ``test/test_types.c``

