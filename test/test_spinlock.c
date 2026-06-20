/**
 * @file test_spinlock.c
 * @brief Tests for the ticket spinlock primitives.
 *
 * Contention testing requires multiple cores (verified by make run);
 * these tests verify correctness of the lock state machine and the
 * interrupt-safe wrappers on the BSP alone.
 */

#include "test.h"
#include "spinlock.h"

uint32_t test_spinlock(void)
{
    TEST_BEGIN("spinlock");

    // ── Static initializer ──────────────────────────────
    {
        spinlock_t lock = SPINLOCK_INIT;
        TEST_ASSERT(!spin_is_locked(&lock), "SPINLOCK_INIT is unlocked");
        TEST_ASSERT_EQ(lock.owner, 0, "init owner == 0");
        TEST_ASSERT_EQ(lock.next, 0, "init next == 0");
    }

    // ── Lock / unlock ───────────────────────────────────
    {
        spinlock_t lock = SPINLOCK_INIT;

        spin_lock(&lock);
        TEST_ASSERT(spin_is_locked(&lock), "locked after spin_lock");
        TEST_ASSERT_EQ(lock.next, 1, "next incremented");
        TEST_ASSERT_EQ(lock.owner, 0, "owner unchanged while held");

        spin_unlock(&lock);
        TEST_ASSERT(!spin_is_locked(&lock), "unlocked after spin_unlock");
        TEST_ASSERT_EQ(lock.owner, 1, "owner advanced");
        TEST_ASSERT_EQ(lock.next, 1, "next unchanged by unlock");
    }

    // ── Multiple lock / unlock cycles ───────────────────
    {
        spinlock_t lock = SPINLOCK_INIT;

        for (int i = 0; i < 10; ++i)
        {
            spin_lock(&lock);
            TEST_ASSERT(spin_is_locked(&lock), "locked in cycle");
            spin_unlock(&lock);
            TEST_ASSERT(!spin_is_locked(&lock), "unlocked in cycle");
        }

        TEST_ASSERT_EQ(lock.owner, 10, "owner after 10 cycles");
        TEST_ASSERT_EQ(lock.next, 10, "next after 10 cycles");
    }

    // ── trylock on free lock ────────────────────────────
    {
        spinlock_t lock = SPINLOCK_INIT;

        bool got = spin_trylock(&lock);
        TEST_ASSERT(got, "trylock succeeds on free lock");
        TEST_ASSERT(spin_is_locked(&lock), "locked after trylock");

        spin_unlock(&lock);
        TEST_ASSERT(!spin_is_locked(&lock), "unlocked after trylock cycle");
    }

    // ── trylock on held lock ────────────────────────────
    {
        spinlock_t lock = SPINLOCK_INIT;

        spin_lock(&lock);
        bool got = spin_trylock(&lock);
        TEST_ASSERT(!got, "trylock fails on held lock");

        spin_unlock(&lock);
    }

    // ── irqsave / irqrestore ────────────────────────────
    {
        spinlock_t lock = SPINLOCK_INIT;

        // Read current interrupt flag state
        uint64_t rflags_before;
        __asm__ volatile("pushfq; popq %0" : "=r"(rflags_before));
        bool if_before = (rflags_before & (1 << 9)) != 0;

        uint64_t flags;
        spin_lock_irqsave(&lock, &flags);

        // Interrupts should be disabled while holding the lock
        uint64_t rflags_during;
        __asm__ volatile("pushfq; popq %0" : "=r"(rflags_during));
        bool if_during = (rflags_during & (1 << 9)) != 0;
        TEST_ASSERT(!if_during, "interrupts disabled under irqsave");

        TEST_ASSERT(spin_is_locked(&lock), "locked under irqsave");

        spin_unlock_irqrestore(&lock, flags);

        // Interrupt flag should be restored to its previous state
        uint64_t rflags_after;
        __asm__ volatile("pushfq; popq %0" : "=r"(rflags_after));
        bool if_after = (rflags_after & (1 << 9)) != 0;
        TEST_ASSERT_EQ(if_after, if_before,
                       "interrupt flag restored after irqrestore");
    }

    TEST_END();
}

