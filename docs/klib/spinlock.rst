======================================
Synchronization: Ticket Spinlocks
======================================

Overview
========

In eMMK's symmetric multiprocessing (SMP) environment, **ticket
spinlocks** (``spinlock_t``) provide low-overhead, fair mutual exclusion
for kernel data structures. Unlike naive test-and-set or
test-and-test-and-set locks, a ticket spinlock enforces Strict First-In,
First-Out (FIFO) ordering, completely eliminating reader/writer starvation
under high core contention.

Design
======

Data Layout
-----------
The ``spinlock_t`` structure packs two 16-bit counters into a single
32-bit aligned word:

.. code-block:: c

   typedef struct
   {
       volatile uint16_t owner;  ///< Ticket currently being served.
       volatile uint16_t next;   ///< Next ticket to issue.
   } spinlock_t;

- **State Check:** The lock is **unlocked** when ``owner == next``. The
  lock is **locked** when ``owner != next``.
- **Capacity:** The 16-bit counter width permits up to 65,535 concurrent
  waiters before integer wrapping occurs—far exceeding eMMK's maximum
  supported configuration of :math:`\le 16` cores (``SMP_MAX_CORES``).

Ticket Lock Mechanics
---------------------

.. code-block:: text

   Initial State (Free):          Core 0 acquires:               Core 1 acquires:
   ┌───────────────┐             ┌───────────────┐              ┌───────────────┐
   │ owner = 0     │             │ owner = 0     │              │ owner = 0     │
   │ next  = 0     │             │ next  = 1     │              │ next  = 2     │
   └───────────────┘             └───────────────┘              └───────────────┘
   [ owner == next ]             [ Core 0 ticket = 0 ]          [ Core 1 ticket = 1 ]
   (Lock Available)              (Serviced immediately)         (Spins on owner == 1)

1. **Acquire** (``spin_lock``): The acquiring core atomically fetches and
   increments the ``next`` counter using x86-64 ``lock xaddw``. The
   returned value is the core's reserved ticket. The core then spins until
   the ``owner`` field equals its ticket number.
2. **Release** (``spin_unlock``): The holding core increments the ``owner``
   field. This instantly signals the next core in line (whose ticket
   matches the updated ``owner`` value) to exit its spin loop.

Hardware Optimization & Memory Ordering
========================================

Hardware Pipeline Management (``PAUSE``)
----------------------------------------
Inside the spin-wait loop, eMMK executes the x86 ``PAUSE`` instruction:

.. code-block:: c

   while (lock->owner != ticket)
       __asm__ volatile("pause" ::: "memory");

Executing ``PAUSE`` provides two crucial hardware benefits:

- **Pipeline De-escalation:** Prevents memory order violations upon
  exiting the loop, preventing the severe CPU pipeline flushes that occur
  when speculative execution guesses incorrectly during lock releases.
- **Power Efficiency:** Reduces processor power consumption and thermal
  output during extended contention cycles.

Memory Barriers & Store Visibility
----------------------------------
To prevent compiler instruction reordering from bleeding critical section
writes past the lock release, ``spin_unlock()`` issues a compiler barrier
before modifying ``owner``:

.. code-block:: c

   static inline void spin_unlock(spinlock_t *lock)
   {
       barrier();      // Prevent compiler from moving stores past this point
       lock->owner++;  // Store-release
   }

On standard x86-64 memory architecture (TSO — Total Store Order), scalar
writes are strictly ordered with respect to other writes, making an
explicit hardware bus lock unnecessary during release.

Interrupt Safety
================

Deadlock Hazard
---------------
If a core acquires a spinlock in normal kernel code and an interrupt fires
on that **same core**, an interrupt handler attempting to grab the same
lock will enter an infinite spin loop waiting for the lock to release.
Because the interrupted thread cannot resume until the handler finishes,
the system enters an unrecoverable single-core deadlock.

Disabling Interrupts (``irqsave`` / ``irqrestore``)
---------------------------------------------------
To prevent deadlocks, any lock accessed by both normal kernel context and
interrupt context **must** use the interrupt-safe variants:

- **spin_lock_irqsave():** Pushes ``RFLAGS`` onto the stack, reads it,
  executes ``cli`` (Clear Interrupt Flag), and then acquires the ticket
  lock.
- **spin_unlock_irqrestore():** Releases the lock and restores ``RFLAGS``
  to its exact state prior to acquiring the lock (re-enabling interrupts
  only if they were enabled beforehand).

API Reference
=============

Basic Locking
-------------

.. c:function:: static inline void spin_lock(spinlock_t \*lock)

   Atomically claims a ticket using ``lock xaddw`` and spins until
   ``owner == ticket``. 

.. c:function:: static inline void spin_unlock(spinlock_t \*lock)

   Enforces a compiler barrier and increments ``owner``, allowing the next
   waiter to proceed.

Interrupt-Safe Locking
----------------------

.. c:function:: static inline void spin_lock_irqsave(spinlock_t \*lock, uint64_t \*flags)

   Saves the local CPU's ``RFLAGS`` register to ``*flags``, executes
   ``cli`` to disable interrupts, and acquires the lock.

.. c:function:: static inline void spin_unlock_irqrestore(spinlock_t \*lock, uint64_t flags)

   Releases the spinlock and restores the CPU's ``RFLAGS`` from ``flags``.

Non-Blocking & Diagnostics
--------------------------

.. c:function:: static inline bool spin_trylock(spinlock_t \*lock)

   Attempts a single non-blocking acquire using atomic compare-and-swap
   (``lock cmpxchgw``). Returns ``true`` if successfully acquired, or
   ``false`` if the lock was busy. Useful in non-blocking diagnostic paths
   such as emergency serial panic logs.

.. c:function:: static inline bool spin_is_locked(spinlock_t \*lock)

   Returns ``true`` if ``owner != next`` (lock is currently held), without
   modifying any lock counters.

Utility Routines
----------------

.. c:function:: static inline uint64_t irq_save(void)

   Saves ``RFLAGS`` via ``pushfq``/``popq`` and executes ``cli``. Returns
   the original flags word.

.. c:function:: static inline void irq_restore(uint64_t flags)

   Restores ``RFLAGS`` using ``pushq``/``popfq``.

References
==========

- `Intel SDM Vol. 2B
  <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html>`_
  — ``PAUSE``, ``XADD``, ``CMPXCHG`` Instruction Set References.
- `Intel SDM Vol. 3
  <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html>`_
  — §9.1 (locked atomic operations)
- **Algorithms for Scalable Synchronization on Shared-Memory
  Multiprocessors:** John M. Mellor-Crummey and Michael L. Scott (1991)

Related Files
=============

- ``include/spinlock.h``
- ``include/types.h``

