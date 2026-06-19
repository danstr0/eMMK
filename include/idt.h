/**
 * @file idt.h
 * @brief Interrupt Descriptor Table (IDT) interface.
 *
 * The x86-64 IDT has 256 entries, each a 16-byte gate descriptor:
 * - Vectors 0–31:    CPU exceptions (divide error, page fault, etc.)
 * - Vectors 32–47:   Legacy PIC IRQs (remapped from 0–15)
 * - Vectors 48–255:  Available for MSI, APIC, software interrupts
 *
 * Each gate descriptor contains the handler address (split across three
 * fields), the code segment selector, gate type, DPL, and an optional
 * IST (Interrupt Stack Table) index for automatic stack switching.
 *
 * @par Interrupt flow
 * 1. CPU pushes SS, RSP, RFLAGS, CS, RIP (and error code if applicable)
 * 2. ISR stub (isr.asm) pushes a dummy error code (if needed) and the
 *    vector number, then saves all general-purpose registers.
 * 3. Stub calls the C handler with a pointer to the @c interrupt_frame.
 * 4. Stub restores registers, removes error code / vector, and executes
 *    IRETQ.
 */

#ifndef EMMK_IDT_H
#define EMMK_IDT_H

#include "types.h"

/// Total number of IDT entries.
#define IDT_ENTRIES  256

/// @name Exception vectors (0-31, defined by the CPU)
/// @{
#define EXCEPTION_DE   0  ///< Divide Error.
#define EXCEPTION_DB   1  ///< Debug.
#define EXCEPTION_NMI  2  ///< Non-Maskable Interrupt.
#define EXCEPTION_BP   3  ///< Breakpoint (INT3).
#define EXCEPTION_OF   4  ///< Overflow (INTO).
#define EXCEPTION_BR   5  ///< BOUND Range Exceeded.
#define EXCEPTION_UD   6  ///< Invalid Opcode.
#define EXCEPTION_NM   7  ///< Device Not Available.
#define EXCEPTION_DF   8  ///< Double Fault (error code: always 0).
#define EXCEPTION_CSO  9  ///< Coprocessor Segment Overrun (reserved).
#define EXCEPTION_TS  10  ///< Invalid TSS (error code).
#define EXCEPTION_NP  11  ///< Segment Not Present (error code).
#define EXCEPTION_SS  12  ///< Stack Fault (error code).
#define EXCEPTION_GP  13  ///< General Protection (error code).
#define EXCEPTION_PF  14  ///< Page-Fault (error code).
//                    15  Intel reserved
#define EXCEPTION_MF  16  ///< x87 FPU Floating-Point Error.
#define EXCEPTION_AC  17  ///< Alignment Check (error code).
#define EXCEPTION_MC  18  ///< Machine-Check.
#define EXCEPTION_XM  19  ///< SIMD Floating-Point.
#define EXCEPTION_VE  20  ///< Virtualization Exception.
#define EXCEPTION_CP  21  ///< Control Protection (error code).
//                 22-31  Intel reserved
//                32-255  User-defined interrupts

#define EXCEPTION_COUNT  32
/// @}

/// @name IRQ vectors (remapped to 32-47)
/// @{
#define IRQ_BASE   32
#define IRQ_COUNT  16

#define IRQ_TIMER     0  ///< Programmable Interval Timer.
#define IRQ_KEYBOARD  1  ///< Keyboard.
#define IRQ_CASCADE   2  ///< Cascade (used internally by PICs).
#define IRQ_COM2      3  ///< COM2 (if enabled).
#define IRQ_COM1      4  ///< COM1 (if enabled).
#define IRQ_LPT2      5  ///< LPT2 (if enabled).
#define IRQ_FLOPPY    6  ///< Floppy disk.
#define IRQ_LPT1      7  ///< LPT1 / spurious.
#define IRQ_RTC       8  ///< CMOS Real-Time Clock (if enabled).
#define IRQ_FREE1     9  ///< Free for peripherals / legacy SCSI / NIC.
#define IRQ_FREE2    10  ///< Free.
#define IRQ_FREE3    11  ///< Free.
#define IRQ_MOUSE    12  ///< PS/2 Mouse.
#define IRQ_FPU      13  ///< FPU / Coprocessor / Inter-processor.
#define IRQ_ATA1     14  ///< Primary ATA Hard Disk.
#define IRQ_ATA2     15  ///< Secondary ATA Hard Disk.

/// Convert a PIC IRQ number (0-15) to its IDT vector (32-47).
#define IRQ_VECTOR(irq)  ((irq) + IRQ_BASE)
/// @}

/// @name Gate descriptor types and attributes
/// @{

// The type_attr byte in an IDT gate descriptor:
//   Bit 7:    Present (P)
//   Bits 6-5: DPL (Descriptor Privilege Level)
//   Bit 4:    Always 0 for system descriptors
//   Bits 3-0: Gate type
#define IDT_PRESENT    (1 << 7)
#define IDT_DPL0       (0 << 5)  ///< Kernel mode only.
#define IDT_DPL3       (3 << 5)  ///< User mode can invoke via INT.
#define IDT_TYPE_INT   0x0E      ///< Interrupt gate (clears IF).
#define IDT_TYPE_TRAP  0x0F      ///< Trap gate (IF unchanged).

/// Common gate attribute combinations.
#define IDT_GATE_INTERRUPT  (IDT_PRESENT | IDT_DPL0 | IDT_TYPE_INT)
#define IDT_GATE_TRAP       (IDT_PRESENT | IDT_DPL0 | IDT_TYPE_TRAP)
#define IDT_GATE_USER       (IDT_PRESENT | IDT_DPL3 | IDT_TYPE_INT)   ///< Unused.
/// @}

/// @name IDT structures
/// @{

/// A single 16-byte IDT gate descriptor (x86-64 format).
struct idt_entry
{
    uint16_t offset_low;   ///< Handler address bits 0-15.
    uint16_t selector;     ///< Code segment selector (GDT offset).
    uint8_t  ist;          ///< IST index (bits 0-2), 0 = no stack switch.
    uint8_t  type_attr;    ///< Gate type, DPL, and present bit.
    uint16_t offset_mid;   ///< Handler address bits 16-31.
    uint32_t offset_high;  ///< Handler address bits 32-63.
    uint32_t reserved;     ///< Must be zero.
} PACKED;

/// LIDT descriptor: 2-byte limit + 8-byte base address.
struct idt_ptr
{
    uint16_t limit;  ///< Size of the IDT in bytes, minus one.
    uint64_t base;   ///< Virtual address of the IDT.
} PACKED;

/// Register state pushed by CPU + ISR stub.
struct interrupt_frame
{
    // Pushed by ISR stub (general-purpose registers)
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;

    // Pushed by ISR stub (interrupt metadata)
    uint64_t vector;      ///< Interrupt vector number (0-255).
    uint64_t error_code;  ///< CPU error code, or 0 if none.

    // Pushed by CPU automatically
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};
/// @}

/// @name Handler type
/// @{

/**
 * @brief C-level interrupt handler function.
 *
 * Called by the assembly ISR stub with interrupts disabled (for interrupt
 * gates). The handler receives the full register state and can inspect
 * or modify it before the stub restores registers and returns via IRETQ.
 */
typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);
/// @}

/// @name API
/// @{

/**
 * @brief Build and load the IDT.
 *
 * Registers assembly stubs for all 256 vectors, installs default
 * exception handlers for vectors 0-31, and loads the IDT via LIDT.
 *
 * @note Must be called before enabling interrupts (STI).
 */
void idt_init(void);

/**
 * @brief Register a C handler for an interrupt vector.
 *
 * @param vector   Interrupt vector (0-255).
 * @param handler  Handler function, or @c NULL to unregister.
 *
 * The handler is called with interrupts disabled (interrupt gate).
 * To register a handler for a PIC IRQ, use @c IRQ_VECTOR(irq) as
 * the vector.
 */
void idt_set_handler(uint8_t vector, interrupt_handler_t handler);

/**
 * @brief Directly configure an IDT gate descriptor.
 *
 * @param vector     Interrupt vector (0-255).
 * @param handler    Handler entry point address.
 * @param selector   Code segment selector (e.g., 0x08 for kernel CS).
 * @param type_attr  Gate type and attributes (e.g., @c IDT_GATE_INTERRUPT).
 * @param ist        IST index (0 = no stack switch, 1-7 = IST entry).
 *
 * @note Low-level interface — most callers should use
 *       @c idt_set_handler() instead.
 */
void idt_set_gate(uint8_t vector, uint64_t handler, uint16_t selector,
                  uint8_t type_attr, uint8_t ist);
/// @}

/// @name Debug
/// @{

/// Human-readable exception names indexed by vector (0-31).
extern const char *exception_names[EXCEPTION_COUNT];
/// @}

#endif // EMMK_IDT_H

