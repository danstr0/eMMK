/**
 * @file apic.c
 * @brief Local APIC and I/O APIC implementation.
 */

#include "apic.h"
#include "vmm.h"
#include "io.h"
#include "pit.h"
#include "serial.h"
#include "idt.h"
#include "pic.h"

// ── MSR helpers ────────────────────────────────────────────────────────

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t low  = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

// ── CPUID detection ────────────────────────────────────────────────────

/// Check CPUID.01H:EDX bit 9 (APIC on-chip).
static bool apic_available(void)
{
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    return (edx & (1 << 9)) != 0;
}

// ── Local APIC state ───────────────────────────────────────────────────

/// Virtual base address of the Local APIC register page.
static volatile uint32_t *lapic_base;

/// Calibrated APIC timer frequency (ticks per second).
static uint32_t lapic_timer_freq;

/// Total timer ticks since boot (incremented by the timer handler).
static volatile uint64_t timer_ticks;

// ── Local APIC register access ─────────────────────────────────────────

uint32_t lapic_read(uint32_t reg)
{
    return lapic_base[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t value)
{
    lapic_base[reg / 4] = value;
}

bool lapic_is_ready(void)
{
    return lapic_base != NULL;
}

// ── Local APIC core functions ──────────────────────────────────────────

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_id(void)
{
    return lapic_read(LAPIC_ID) >> 24;
}

// ── PIT-based calibration ──────────────────────────────────────────────

#define CALIBRATION_MS  10

/**
 * @brief Measure APIC timer frequency using PIT channel 2.
 *
 * Programs PIT channel 2 for a one-shot countdown of @c CALIBRATION_MS
 * milliseconds, then counts how many APIC timer ticks elapse.
 *
 * @return APIC timer ticks per second.
 */
static uint32_t calibrate_apic_timer(void)
{
    // Set APIC timer divider to 16
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);

    pit_oneshot_start(PIT_MS_TO_COUNT(CALIBRATION_MS));

    // Start the APIC timer with the maximum initial count
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

    while (!pit_oneshot_done())
        cpu_relax();

    // Read how many APIC ticks elapsed
    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);

    // Stop the APIC timer
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    pit_oneshot_stop();

    // Scale elapsed ticks to a per-second frequency
    uint32_t freq = elapsed * (1000 / CALIBRATION_MS);

    kprintf("[APIC] Timer calibration: %lu ticks in %u ms -> %lu ticks/sec\n",
            (uint64_t)elapsed, CALIBRATION_MS, (uint64_t)freq);

    return freq;
}

// ── Interrupt handlers ─────────────────────────────────────────────────

/// APIC timer interrupt handler.
static void timer_handler(struct interrupt_frame *frame)
{
    (void)frame;
    timer_ticks++;
    lapic_eoi();
}

/**
 * @brief APIC error interrupt handler.
 *
 * Reads and logs the Error Status Register contents, then acknowledges
 * the interrupt. The ESR is write-to-read: a dummy write latches the
 * current error bits so the subsequent read returns them.
 */
static void error_handler(struct interrupt_frame *frame)
{
    (void)frame;
    lapic_write(LAPIC_ESR, 0);
    uint32_t esr = lapic_read(LAPIC_ESR);
    kprintf("[APIC] Error interrupt: ESR=0x%lx\n", (uint64_t)esr);
    lapic_eoi();
}

/**
 * @brief APIC spurious interrupt handler.
 *
 * A genuine spurious interrupt does not set an in-service bit, so it
 * must not be acknowledged. Never send a fixed IPI on this vector: it
 * @e would set the in-service bit, and without an EOI it would then block
 * every interrupt on the target core.
 */
static void spurious_handler(struct interrupt_frame *frame)
{
    (void)frame;
}

/**
 * @brief Wake-up IPI handler.
 *
 * The IPI exists only to bring a core out of @c hlt; the woken core
 * re-checks whatever condition it was waiting for.
 */
static void wake_handler(struct interrupt_frame *frame)
{
    (void)frame;
    lapic_eoi();
}

// ── Local APIC timer API ───────────────────────────────────────────────

void lapic_timer_init(uint32_t hz)
{
    if (lapic_timer_freq == 0)
        lapic_timer_freq = calibrate_apic_timer();

    uint32_t count = lapic_timer_freq / hz;
    if (count == 0)
        count = 1;

    kprintf("[APIC] Timer: %lu Hz, initial count = %lu\n",
            (uint64_t)hz, (uint64_t)count);

    // Register the timer interrupt handler
    idt_set_handler(APIC_TIMER_VECTOR, timer_handler);

    // Configure: periodic mode, divider 16
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, APIC_TIMER_VECTOR | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_ICR, count);
}

void lapic_timer_stop(void)
{
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_ICR, 0);
}

uint64_t lapic_timer_get_ticks(void)
{
    return timer_ticks;
}

// ── Inter-processor interrupts ─────────────────────────────────────────────

/// Spin until the ICR delivery status bit clears.
static inline void lapic_ipi_wait(void)
{
    while (lapic_read(LAPIC_ICR_LOW) & LAPIC_ICR_PENDING)
        cpu_relax();
}

void lapic_send_ipi(uint32_t apic_id, uint8_t vector)
{
    lapic_ipi_wait();
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_FIXED
                | LAPIC_ICR_DEST_NONE | LAPIC_ICR_LEVEL_ASSERT);
}

void lapic_send_ipi_all(uint8_t vector)
{
    lapic_ipi_wait();
    lapic_write(LAPIC_ICR_LOW, vector | LAPIC_ICR_FIXED
                | LAPIC_ICR_DEST_OTHERS | LAPIC_ICR_LEVEL_ASSERT);
}

void lapic_send_nmi_all(void)
{
    lapic_write(LAPIC_ICR_LOW, LAPIC_ICR_NMI | LAPIC_ICR_DEST_OTHERS
                | LAPIC_ICR_LEVEL_ASSERT);
}

// ── Local APIC initialization ──────────────────────────────────────────────

void lapic_init(void)
{
    kprintf("[APIC] Initializing Local APIC...\n");

    if (!apic_available())
    {
        kprintf("[APIC] ERROR: No APIC detected (CPUID.01H:EDX.bit9 = 0)\n");
        return;
    }

    // The legacy PIC should already have been initialized (remapped) and
    // disabled by the caller. Mask all lines as a safety measure.
    pic_disable();

    // Verify the APIC is enabled; enable it if not
    uint64_t apic_msr = rdmsr(MSR_IA32_APIC_BASE);
    uint64_t base_phys = apic_msr & APIC_BASE_ADDR_MASK;

    if (!(apic_msr & APIC_BASE_ENABLE))
    {
        apic_msr |= APIC_BASE_ENABLE;
        wrmsr(MSR_IA32_APIC_BASE, apic_msr);
        kprintf("[APIC] Enabled via MSR\n");
    }

    // Map the APIC register page (uncached). All cores share the same
    // physical address and therefore this one mapping.
    lapic_base = (volatile uint32_t *)vmm_map_mmio(base_phys, PAGE_SIZE);
    if (!lapic_base)
    {
        kprintf("[APIC] ERROR: Failed to map APIC registers "
                "at phys 0x%lx\n", base_phys);
        return;
    }

    kprintf("[APIC] Base: phys 0x%lx, virt %p%s\n",
            base_phys, (void *)lapic_base,
            (apic_msr & APIC_BASE_BSP) ? " (BSP)" : "");

    // Enable the APIC via the Spurious Vector Register
    lapic_write(LAPIC_SVR, APIC_SPURIOUS_VECTOR | LAPIC_SVR_ENABLE);

    // Accept all interrupt priorities
    lapic_write(LAPIC_TPR, 0);

    // Mask all LVT entries - drivers will unmask as needed
    lapic_write(LAPIC_LVT_TIMER,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PERF,    LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0,   LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1,   LAPIC_LVT_MASKED);

    // Wire up the error LVT to the error handler (unmasked)
    idt_set_handler(APIC_ERROR_VECTOR, error_handler);
    lapic_write(LAPIC_LVT_ERROR, APIC_ERROR_VECTOR);

    // Register spurious and wake-up IPI handlers
    idt_set_handler(APIC_SPURIOUS_VECTOR, spurious_handler);
    idt_set_handler(APIC_WAKE_VECTOR, wake_handler);

    // Clear error status - writing twice ensures any latched errors
    // are cleared
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    // Clear any pending interrupt
    lapic_eoi();

    // Report version info
    uint32_t version = lapic_read(LAPIC_VERSION);
    kprintf("[APIC] APIC ID %lu, version 0x%lx, %lu LVT entries\n",
            (uint64_t)lapic_id(),
            (uint64_t)(version & 0xFF),
            (uint64_t)(((version >> 16) & 0xFF) + 1));
}

// ── I/O APIC ───────────────────────────────────────────────────────────

/// Virtual base address of the I/O APIC register window.
static volatile uint32_t *ioapic_base;

uint32_t ioapic_read(uint32_t reg)
{
    ioapic_base[IOAPIC_REG_SELECT / 4] = reg;
    return ioapic_base[IOAPIC_REG_DATA / 4];
}

void ioapic_write(uint32_t reg, uint32_t value)
{
    ioapic_base[IOAPIC_REG_SELECT / 4] = reg;
    ioapic_base[IOAPIC_REG_DATA / 4]   = value;
}

/**
 * @brief Read a 64-bit I/O APIC redirection entry.
 * @param irq  Redirection table index (0-based).
 */
static uint64_t ioapic_read_redir(uint8_t irq)
{
    uint32_t reg = IOAPIC_REDTBL + irq * 2;
    uint32_t low  = ioapic_read(reg);
    uint32_t high = ioapic_read(reg + 1);
    return ((uint64_t)high << 32) | low;
}

/**
 * @brief Write a 64-bit I/O APIC redirection entry.
 * @param irq    Redirection table index (0-based).
 * @param entry  64-bit redirection entry value.
 */
static void ioapic_write_redir(uint8_t irq, uint64_t entry)
{
    uint32_t reg = IOAPIC_REDTBL + irq * 2;
    ioapic_write(reg,     (uint32_t)(entry & 0xFFFFFFFF));
    ioapic_write(reg + 1, (uint32_t)(entry >> 32));
}

void ioapic_init(uint64_t base_phys)
{
    kprintf("[IOAPIC] Initializing at phys 0x%lx...\n", base_phys);

    // Map the I/O APIC register window (uncached)
    ioapic_base = (volatile uint32_t *)vmm_map_mmio(base_phys, PAGE_SIZE);
    if (!ioapic_base)
    {
        kprintf("[IOAPIC] ERROR: Failed to map I/O APIC registers\n");
        return;
    }

    // Read I/O APIC identification and capabilities
    uint32_t id      = (ioapic_read(IOAPIC_ID) >> 24) & 0x0F;
    uint32_t version = ioapic_read(IOAPIC_VERSION);
    uint32_t max_irq = ((version >> 16) & 0xFF);

    kprintf("[IOAPIC] ID %lu, version 0x%lx, %lu redirection entries\n",
            (uint64_t)id, (uint64_t)(version & 0xFF),
            (uint64_t)(max_irq + 1));

    // Mask all redirection entries
    for (uint8_t i = 0; i <= max_irq && i < IOAPIC_MAX_REDIR; ++i)
    {
        uint64_t entry = ioapic_read_redir(i);
        entry |= IOAPIC_REDIR_MASKED;
        ioapic_write_redir(i, entry);
    }

    kprintf("[IOAPIC] All entries masked\n");
}

void ioapic_route_irq(uint8_t irq, uint8_t vector, uint32_t apic_id)
{
    // Build a redirection entry:
    //   - Fixed delivery mode
    //   - Physical destination mode
    //   - Edge triggered, active high
    //   - Not masked (enabled)
    //   - Destination in bits 56-59 (APIC ID)
    uint64_t entry = 0;
    entry |= vector;
    entry |= IOAPIC_DELMODE_FIXED;
    entry |= ((uint64_t)apic_id << 56);

    ioapic_write_redir(irq, entry);

    kprintf("[IOAPIC] Routed IRQ %u -> vector %u, APIC ID %lu\n",
            (uint32_t)irq, (uint32_t)vector, (uint64_t)apic_id);
}

void ioapic_mask_irq(uint8_t irq)
{
    uint64_t entry = ioapic_read_redir(irq);
    entry |= IOAPIC_REDIR_MASKED;
    ioapic_write_redir(irq, entry);
}

void ioapic_unmask_irq(uint8_t irq)
{
    uint64_t entry = ioapic_read_redir(irq);
    entry &= ~IOAPIC_REDIR_MASKED;
    ioapic_write_redir(irq, entry);
}

