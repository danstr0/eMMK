/**
 * @file apic.h
 * @brief Local APIC and I/O APIC interface.
 *
 * The APIC (Advanced Programmable Interrupt Controller) subsystem replaces
 * the legacy 8259 PIC on modern x86 systems. It consists of:
 *
 * @par Local APIC (one per CPU core)
 * - Memory-mapped registers at a configurable physical address
 *   (default @c 0xFEE00000, confirmed via the @c IA32_APIC_BASE MSR).
 * - Handles: local timer, IPI, LINT0/LINT1, error, thermal,
 *   performance counters.
 * - Sends EOI to acknowledge handled interrupts.
 *
 * @par I/O APIC (typically one per system, more on multi-socket)
 * - Routes external device interrupts (NIC, disk, etc.) to
 *   Local APICs.
 * - Memory-mapped registers at an address from the ACPI MADT.
 * - Programmable routing table (24 entries on standard I/O APIC).
 *
 * @par Initialization order
 * 1. @c pic_disable()      — mask all legacy PIC IRQs.
 * 2. @c lapic_init()       — enable Local APIC, set spurious vector.
 * 3. @c ioapic_init()      — configure I/O APIC routing table.
 * 4. @c lapic_timer_init() — set up periodic timer.
 */

#ifndef EMMK_APIC_H
#define EMMK_APIC_H

#include "types.h"

/// @name IA32_APIC_BASE MSR (0x1B)
/// @{

/// MSR address for the APIC base configuration.
#define MSR_IA32_APIC_BASE   0x1B

/// Bootstrap processor flag (bit 8, read-only).
#define APIC_BASE_BSP        (1ULL << 8)

/// x2APIC mode enable (bit 10).
#define APIC_BASE_X2APIC     (1ULL << 10)

/// APIC global enable flag (bit 11).
#define APIC_BASE_ENABLE     (1ULL << 11)

/// Bits 12-51: Physical base address of the APIC registers.
#define APIC_BASE_ADDR_MASK  0x000FFFFFFFFFF000ULL
/// @}

/// @name Fallback virtual addresses
/// @{
#define LAPIC_FALLBACK_VADDR   0xFFFFFFFFFFE00000ULL
#define IOAPIC_FALLBACK_VADDR  0xFFFFFFFFFFE01000ULL
/// @}

/// @name Local APIC default base address
/// @{

/**
 * Default physical address of the Local APIC register page.
 * The actual address is read from @c MSR_IA32_APIC_BASE during init.
 */
#define LAPIC_BASE_PHYS_DEFAULT  0xFEE00000
/// @}

/// @name Local APIC register offsets
/// @{
#define LAPIC_ID           0x020  ///< (RW) Local APIC ID.
#define LAPIC_VERSION      0x030  ///< (RO) Version.
#define LAPIC_TPR          0x080  ///< (RW) Task Priority.
#define LAPIC_APR          0x090  ///< (RO) Arbitration Priority.
#define LAPIC_PPR          0x0A0  ///< (RO) Processor Priority.
#define LAPIC_EOI          0x0B0  ///< (WO) End of Interrupt.
#define LAPIC_RRD          0x0C0  ///< (RO) Remote Read.
#define LAPIC_LDR          0x0D0  ///< (RW) Logical Destination.
#define LAPIC_DFR          0x0E0  ///< (RW) Destination Format.
#define LAPIC_SVR          0x0F0  ///< (RW) Spurious Interrupt Vector.
#define LAPIC_ISR          0x100  ///< (RO) In-Service (8 x 32-bit).
#define LAPIC_TMR          0x180  ///< (RO) Trigger Mode (8 x 32-bit).
#define LAPIC_IRR          0x200  ///< (RO) Interrupt Request (8 x 32-bit).
#define LAPIC_ESR          0x280  ///< (RW) Error Status.
#define LAPIC_CMCI         0x2F0  ///< (RW) Corrected Machine Check Interrupt.
#define LAPIC_ICR_LOW      0x300  ///< (RW) Interrupt Command (low).
#define LAPIC_ICR_HIGH     0x310  ///< (RW) Interrupt Command (high).
#define LAPIC_LVT_TIMER    0x320  ///< (RW) LVT Timer.
#define LAPIC_LVT_THERMAL  0x330  ///< (RW) LVT Thermal Sensor.
#define LAPIC_LVT_PERF     0x340  ///< (RW) LVT Performance Monitoring.
#define LAPIC_LVT_LINT0    0x350  ///< (RW) LVT LINT0.
#define LAPIC_LVT_LINT1    0x360  ///< (RW) LVT LINT1.
#define LAPIC_LVT_ERROR    0x370  ///< (RW) LVT Error.
#define LAPIC_TIMER_ICR    0x380  ///< (RW) Timer Initial Count.
#define LAPIC_TIMER_CCR    0x390  ///< (RO) Timer Current Count.
#define LAPIC_TIMER_DCR    0x3E0  ///< (RW) Timer Divide Configuration.
/// @}

/// @name Spurious Vector Register (SVR) bits
/// @{
#define LAPIC_SVR_ENABLE  (1 << 8)  ///< APIC software enable.
/// @}

/// @name Local Vector Table (LVT) bits
/// @{
#define LAPIC_LVT_MASKED          (1 << 16)  ///< Interrupt masked.
#define LAPIC_LVT_LEVEL           (1 << 15)  ///< Level triggered (vs edge).
#define LAPIC_LVT_PENDING         (1 << 12)  ///< Delivery status: pending.

/// Delivery mode field (bits 10:8) in LVT entries.
#define LAPIC_LVT_DELMODE_FIXED   (0 << 8)
#define LAPIC_LVT_DELMODE_SMI     (2 << 8)
#define LAPIC_LVT_DELMODE_NMI     (4 << 8)
#define LAPIC_LVT_DELMODE_INIT    (5 << 8)
#define LAPIC_LVT_DELMODE_EXTINT  (7 << 8)
/// @}

/// @name Timer mode bits (LVT timer, bits 18:17)
/// @{
#define LAPIC_TIMER_ONESHOT       (0 << 17)
#define LAPIC_TIMER_PERIODIC      (1 << 17)
#define LAPIC_TIMER_TSC_DEADLINE  (2 << 17)
/// @}

/// @name Timer divide values (LAPIC_TIMER_DCR)
/// @{
#define LAPIC_TIMER_DIV_1    0x0B
#define LAPIC_TIMER_DIV_2    0x00
#define LAPIC_TIMER_DIV_4    0x01
#define LAPIC_TIMER_DIV_8    0x02
#define LAPIC_TIMER_DIV_16   0x03
#define LAPIC_TIMER_DIV_32   0x08
#define LAPIC_TIMER_DIV_64   0x09
#define LAPIC_TIMER_DIV_128  0x0A
/// @}

/// @name Interrupt Command Register (ICR) fields
/// @{

// Delivery mode (bits 10:8)
#define LAPIC_ICR_FIXED           (0 << 8)
#define LAPIC_ICR_LOWEST          (1 << 8)
#define LAPIC_ICR_SMI             (2 << 8)
#define LAPIC_ICR_NMI             (4 << 8)
#define LAPIC_ICR_INIT            (5 << 8)
#define LAPIC_ICR_STARTUP         (6 << 8)

// Destination shorthand (bits 19:18)
#define LAPIC_ICR_DEST_NONE       (0 << 18)  ///< Use destination field.
#define LAPIC_ICR_DEST_SELF       (1 << 18)  ///< Send to self.
#define LAPIC_ICR_DEST_ALL        (2 << 18)  ///< All including self.
#define LAPIC_ICR_DEST_OTHERS     (3 << 18)  ///< All excluding self.

// Level and trigger (bits 15:14)
#define LAPIC_ICR_LEVEL_DEASSERT  (0 << 14)
#define LAPIC_ICR_LEVEL_ASSERT    (1 << 14)

// Delivery status (bit 12, read-only)
#define LAPIC_ICR_PENDING         (1 << 12)
/// @}

/// @name Interrupt vector assignments
/// @{

/// APIC timer interrupt vector (first vector after PIC range).
#define APIC_TIMER_VECTOR     48

/// APIC error interrupt vector.
#define APIC_ERROR_VECTOR     49

/// Spurious interrupt vector (traditionally 0xFF).
#define APIC_SPURIOUS_VECTOR  255
/// @}

/// @name Local APIC API — Core
/// @{

/**
 * @brief Detect, enable, and configure the Local APIC.
 *
 * Reads the APIC base address from the @c IA32_APIC_BASE MSR, maps the
 * register page, enables the APIC via the SVR, and masks all LVT
 * entries. The legacy PIC must already be initialized and disabled
 * before calling this function.
 */
void lapic_init(void);

/// @return The current CPU's Local APIC ID.
uint32_t lapic_id(void);

/**
 * @brief Signal End of Interrupt to the Local APIC.
 *
 * Must be called at the end of every APIC-delivered interrupt handler
 * (including timer, I/O APIC-routed, and error interrupts).
 */
void lapic_eoi(void);
/// @}

/// @name Local APIC API — Register access
/// @{

/// Read a 32-bit Local APIC register at @p reg offset.
uint32_t lapic_read(uint32_t reg);

/// Write @p value to a 32-bit Local APIC register at @p reg offset.
void lapic_write(uint32_t reg, uint32_t value);
/// @}

/// @name Local APIC API — Timer
/// @{

/**
 * @brief Configure the APIC timer for periodic interrupts.
 *
 * Calibrates the timer against a known time source (PIT channel 2),
 * then programs periodic mode with the calibrated count. Delivers
 * interrupts on @c APIC_TIMER_VECTOR.
 *
 * @param hz  Desired interrupt frequency in Hz.
 */
void lapic_timer_init(uint32_t hz);

/// Stop the APIC timer by masking the LVT timer entry.
void lapic_timer_stop(void);

/// @return Total timer ticks since boot.
uint64_t lapic_timer_get_ticks(void);
/// @}

/// @name Local APIC API — Inter-processor interrupts
/// @{

/**
 * @brief Send an IPI to a specified CPU.
 * @param apic_id  Target CPU's Local APIC ID.
 * @param vector   Interrupt vector to deliver.
 */
void lapic_send_ipi(uint32_t apic_id, uint8_t vector);

/**
 * @brief Broadcast an IPI to all other CPUs.
 * @param vector  Interrupt vector to deliver.
 */
void lapic_send_ipi_all(uint8_t vector);
/// @}

/**
 * @brief Default I/O APIC base address.
 *
 * @note The actual address comes from the ACPI MADT. This default
 *       covers the common case for single-socket systems.
 */
#define IOAPIC_BASE_PHYS_DEFAULT  0xFEC00000

/// @name I/O APIC register constants
/// @{

// I/O APIC register access ports (memory-mapped offsets)
#define IOAPIC_REG_SELECT       0x00  ///< Register select (write index).
#define IOAPIC_REG_DATA         0x10  ///< Data window (read/write selected).

// I/O APIC registers (written to IOREGSEL)
#define IOAPIC_ID               0x00  ///< I/O APIC ID.
#define IOAPIC_VERSION          0x01  ///< Version.
#define IOAPIC_ARB              0x02  ///< Arbitration ID.
#define IOAPIC_REDTBL           0x10  ///< Redirection table base.

// Number of redirection entries on a standard I/O APIC
#define IOAPIC_MAX_REDIR        24

// Redirection entry bits (64-bit entry, split across two 32-bit regs)
#define IOAPIC_REDIR_MASKED     (1ULL << 16)  ///< Interrupt masked.
#define IOAPIC_REDIR_LEVEL      (1ULL << 15)  ///< Level triggered.
#define IOAPIC_REDIR_ACTIVELOW  (1ULL << 13)  ///< Active low polarity.
#define IOAPIC_REDIR_LOGICAL    (1ULL << 11)  ///< Logical destination mode.

// Delivery modes for redirection entries (bits 10:8)
#define IOAPIC_DELMODE_FIXED    (0ULL << 8)
#define IOAPIC_DELMODE_LOWEST   (1ULL << 8)
#define IOAPIC_DELMODE_SMI      (2ULL << 8)
#define IOAPIC_DELMODE_NMI      (4ULL << 8)
#define IOAPIC_DELMODE_INIT     (5ULL << 8)
#define IOAPIC_DELMODE_EXTINT   (7ULL << 8)
/// @}

/// @name I/O APIC API
/// @{

/**
 * @brief Initialize the I/O APIC.
 *
 * Maps the register page and masks all redirection entries.
 *
 * @param base_phys  Physical address of the I/O APIC registers.
 *
 * @note Must be called after @c lapic_init().
 */
void ioapic_init(uint64_t base_phys);

/**
 * @brief Route an external IRQ to a Local APIC.
 *
 * Configures the redirection entry for @p irq as edge-triggered,
 * active-high, fixed delivery to the specified APIC ID.
 *
 * @param irq      IRQ number (I/O APIC input pin, 0-based).
 * @param vector   IDT vector to deliver.
 * @param apic_id  Destination Local APIC ID.
 */
void ioapic_route_irq(uint8_t irq, uint8_t vector, uint32_t apic_id);

/// Disable (mask) an I/O APIC redirection entry.
void ioapic_mask_irq(uint8_t irq);

/// Enable (unmask) an I/O APIC redirection entry.
void ioapic_unmask_irq(uint8_t irq);

/// Read a 32-bit I/O APIC register.
uint32_t ioapic_read(uint32_t reg);

/// Write a 32-bit I/O APIC register.
void ioapic_write(uint32_t reg, uint32_t value);
/// @}

#endif // EMMK_APIC_H

