/**
 * @file irq.c
 * @brief Interrupt-vector allocator (dynamic range 64-239).
 */

#include "irq.h"
#include "spinlock.h"

// ── State ──────────────────────────────────────────────────────────────

/// Allocation flag per dynamic vector (index = vector - IRQ_DYN_FIRST).
static bool vector_used[IRQ_DYN_COUNT];

/// Context pointer per dynamic vector.
static void *vector_ctx[IRQ_DYN_COUNT];

static spinlock_t irq_lock = SPINLOCK_INIT;

// ── Allocation ─────────────────────────────────────────────────────────

int irq_alloc_vector(interrupt_handler_t handler, void *ctx)
{
    if (!handler)
        return -1;

    int vector = -1;

    uint64_t flags;
    spin_lock_irqsave(&irq_lock, &flags);
    for (int i = 0; i < IRQ_DYN_COUNT; ++i)
        if (!vector_used[i])
        {
            vector_used[i] = true;
            vector_ctx[i] = ctx;
            vector = IRQ_DYN_FIRST + i;
            break;
        }
    spin_unlock_irqrestore(&irq_lock, flags);

    // The context is stored before the handler goes live, so a handler
    // never observes a stale context
    if (vector >= 0)
    {
        barrier();
        idt_set_handler((uint8_t)vector, handler);
    }

    return vector;
}

void irq_free_vector(int vector)
{
    if (vector < IRQ_DYN_FIRST || vector > IRQ_DYN_LAST)
        return;

    int i = vector - IRQ_DYN_FIRST;

    uint64_t flags;
    spin_lock_irqsave(&irq_lock, &flags);
    if (vector_used[i])
    {
        idt_set_handler((uint8_t)vector, NULL);
        barrier();
        vector_ctx[i] = NULL;
        vector_used[i] = false;
    }
    spin_unlock_irqrestore(&irq_lock, flags);
}

// ── Queries ────────────────────────────────────────────────────────────

void *irq_vector_ctx(uint64_t vector)
{
    if (vector < IRQ_DYN_FIRST || vector > IRQ_DYN_LAST)
        return NULL;
    return vector_ctx[vector - IRQ_DYN_FIRST];
}

int irq_vectors_free(void)
{
    int n = 0;

    uint64_t flags;
    spin_lock_irqsave(&irq_lock, &flags);
    for (int i = 0; i < IRQ_DYN_COUNT; ++i)
        if (!vector_used[i])
            n++;
    spin_unlock_irqrestore(&irq_lock, flags);

    return n;
}

