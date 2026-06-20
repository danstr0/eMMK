/**
 * @file spinlock.h
 * @brief Ticket spinlock for SMP Synchronization.
 *
 * A ticket lock guarantees FIFO ordering: each caller takes a "ticket"
 * (an incrementing number) and spins until the lock's "now serving"
 * counter matches its ticket. This prevents starvation, which naive
 * test-and-set locks suffer from under contention.
 *
 * @par Implementation
 * The lock is a pair of 16-bit counters packed into a 32-bit word:
 * - @c next:  the next ticket to be issued (atomically incremented
 *   by each caller).
 * - @c owner: the ticket currently being served (incremented on
 *   unlock).
 *
 * The lock is held when @c next != @c owner. It is free when they
 * are equal. The 16-bit width allows 65535 concurrent waiters before
 * wrapping, which is more than sufficient for ≤ 16 cores.
 *
 * @par Interrupt safety
 * @c spin_lock_irqsave() disables interrupts before acquiring the
 * lock and returns the previous @c RFLAGS value. @c spin_unlock_irqrestore()
 * restores @c RFLAGS after releasing. This prevents deadlock when an
 * interrupt handler on the same core tries to acquire a lock that the
 * interrupted code already holds.
 *
 * Use the @c _irqsave / @c _irqrestore variants for any lock that
 * might be acquired from both normal code and interrupt context.
 * Use the plain @c spin_lock / @c spin_unlock when the lock is never
 * touched from an interrupt handler.
 *
 * @par Performance notes
 * - The spin loop uses @c PAUSE to reduce pipeline pressure and
 *   improve power efficiency.
 * - On x86-64, @c lock xadd provides an atomic fetch-and-add in a 
 *   single instruction.
 * - The compiler barrier in @c spin_unlock ensures all protected
 *   stores are visible before the lock is released.
 *
 * @par Usage
 * @code
 *   static spinlock_t my_lock = SPINLOCK_INIT;
 *
 *   spin_lock(&my_lock);
 *   // critical section
 *   spin_unlock(&my_lock);
 *
 *   // Or with interrupt safety:
 *   uint64_t flags;
 *   spin_lock_irqsave(&my_lock, &flags);
 *   // critical section (interrupts disabled)
 *   spin_unlock_irqrestore(&my_lock, flags);
 * @endcode
 *
 * @see Intel SDM Vol. 2, PAUSE instruction.
 * @see Intel SDM Vol. 3, §8.1 (locked atomic operations)
 */

#ifndef EMMK_SPINLOCK_H
#define EMMK_SPINLOCK_H

#include "types.h"

/// @name Spinlock type
/// @{

/**
 * @brief Ticket spinlock.
 *
 * Both fields start at zero. The lock is free when
 * @c owner == @c next.
 */
typedef struct
{
    volatile uint16_t owner;  ///< Ticket currently being served.
    volatile uint16_t next;   ///< Next ticket to issue.
} spinlock_t;

/// Static initializer for a spinlock (unlocked).
#define SPINLOCK_INIT  { .owner = 0, .next = 0 }
/// @}

/// @name Core lock / unlock
/// @{

/**
 * @brief Acquire the spinlock.
 *
 * Atomically takes a ticket and spins until the lock's owner counter
 * matches. Fair (FIFO) ordering is guaranteed.
 */
static inline void spin_lock(spinlock_t *lock)
{
    // Atomically fetch-and-increment lock->next. The returned value
    // is our ticket. Spin until lock->owner reaches it.
    //
    // lock xaddw atomically does:
    //   tmp = *next;  *next = tmp + 1;  ticket = tmp;
    uint16_t ticket;
    __asm__ volatile(
        "lock xaddw %0, %1"
        : "=r"(ticket), "+m"(lock->next)
        : "0"((uint16_t)1)
        : "memory"
    );

    while (lock->owner != ticket)
        __asm__ volatile("pause" ::: "memory");
}

/**
 * @brief Release the spinlock.
 *
 * Increments the owner counter so the next waiter (with the next
 * ticket number) can proceed. A compiler barrier ensures all stores
 * within the critical section are visible before the release.
 */
static inline void spin_unlock(spinlock_t *lock)
{
    barrier();
    lock->owner++;
}
/// @}

/// @name Interrupt-safe variants
/// @{

/**
 * @brief Save RFLAGS and disable interrupts.
 * @return The previous RFLAGS value (for later restoration).
 */
static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile(
        "pushfq\n"
        "popq %0\n"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

/**
 * @brief Restore RFLAGS (re-enabling interrupts if they were enabled
 *        before @c irq_save).
 */
static inline void irq_restore(uint64_t flags)
{
    __asm__ volatile(
        "pushq %0\n"
        "popfq"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

/**
 * @brief Acquire the lock with interrupts disabled.
 *
 * Saves RFLAGS (capturing the current IF state), disables interrupts,
 * then acquires the lock. The saved flags must be passed to
 * @c spin_unlock_irqrestore() to restore the original interrupt state.
 *
 * @param      lock   Spinlock to acquire.
 * @param[out] flags  Saved RFLAGS value.
 */
static inline void spin_lock_irqsave(spinlock_t *lock, uint64_t *flags)
{
    *flags = irq_save();
    spin_lock(lock);
}

/**
 * @brief Release the lock and restore the saved interrupt state.
 *
 * @param lock   Spinlock to release.
 * @param flags  RFLAGS value from the corresponding @c spin_lock_irqsave.
 */
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
{
    spin_unlock(lock);
    irq_restore(flags);
}
/// @}

/// @name Query
/// @{

/// @return @c true if the lock is currently held by any core.
static inline bool spin_is_locked(spinlock_t *lock)
{
    return lock->owner != lock->next;
}
/// @}

/// @name Debug
/// @{

/**
 * @brief Try to acquire the lock without blocking.
 *
 * Attempts a single atomic compare-and-swap. If the lock is free
 * (owner == next), acquires it and returns @c true. Otherwise
 * returns @c false immediately.
 *
 * Useful for diagnostic paths that should not block (e.g., a panic
 * handler trying to grab the serial lock for output).
 */
static inline bool spin_trylock(spinlock_t *lock)
{
    uint16_t current = lock->owner;
    uint16_t next    = current + 1;

    // cmpxchgw: if lock->next == current, set lock->next = next and
    // return true (ZF set). Otherwise return false.
    bool acquired;
    __asm__ volatile(
        "lock cmpxchgw %2, %1"
        : "=@ccz"(acquired), "+m"(lock->next)
        : "r"(next), "a"(current)
        : "memory"
    );

    if (acquired)
        return true;  // we have 'current' and it's already being served
    return false;
}
/// @}

#endif // EMMK_SPINLOCK_H

