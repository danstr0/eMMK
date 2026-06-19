/**
 * @file pic.h
 * @brief 8259 PIC (Programmable Interrupt Controller).
 *
 * The legacy dual-PIC setup handles hardware IRQs 0–15:
 * - PIC1 (primary):    IRQ 0–7,  I/O ports 0x20–0x21
 * - PIC2 (secondary):  IRQ 8–15, I/O ports 0xA0–0xA1
 *
 * PIC2 is cascaded through PIC1's IRQ 2, so IRQ 2 is not directly
 * usable (it is the cascade line).
 *
 * By default, the BIOS maps PIC IRQs 0–7 to vectors 0x08–0x0F, which
 * overlap CPU exception vectors. @c pic_init() remaps them to vectors
 * 32–47 (@c IRQ_BASE from idt.h) to avoid conflicts.
 *
 * @par Lifecycle
 * 1. @c pic_init()       — remap and mask all IRQs
 * 2. @c pic_clear_mask() — enable specific IRQs as drivers register
 * 3. @c pic_disable()    — called before switching to the APIC
 *
 * @par Spurious IRQs
 * The PIC can generate spurious interrupts on IRQ 7 (PIC1) or IRQ 15
 * (PIC2) due to electrical noise or race conditions. These must be
 * detected by reading the ISR register and handled specially:
 * - Spurious IRQ 7:  do @b not send any EOI.
 * - Spurious IRQ 15: send EOI to PIC1 only (not PIC2).
 * Use @c pic_is_spurious() to check before processing.
 */

#ifndef EMMK_PIC_H
#define EMMK_PIC_H

#include "types.h"

/// @name PIC I/O ports
/// @{
#define PIC1_COMMAND  0x20  ///< Primary PIC command port.
#define PIC1_DATA     0x21  ///< Primary PIC data port.
#define PIC2_COMMAND  0xA0  ///< Secondary PIC command port.
#define PIC2_DATA     0xA1  ///< Secondary PIC data port.
/// @}

/// @name PIC commands
/// @{
#define PIC_EOI       0x20  ///< End of Interrupt.
#define PIC_READ_IRR  0x0A  ///< Read Interrupt Request Register.
#define PIC_READ_ISR  0x0B  ///< Read In-Service Register.
/// @}

/// @name Initialization Command Words (ICW)
/// @{

// ICW1 - sent to command port to begin initialization
#define ICW1_ICW4           0x01  ///< ICW4 will be present.
#define ICW1_SINGLE         0x02  ///< Single PIC (no cascade).
#define ICW1_INTERVAL4      0x04  ///< Call address interval 4 (vs 8).
#define ICW1_LEVEL          0x08  ///< Level triggered (vs edge triggered).
#define ICW1_INIT           0x10  ///< Initialization - required.

// ICW4 - operating mode configuration
#define ICW4_8086           0x01  ///< 8086/88 mode (vs MCS-80/85).
#define ICW4_AUTO_EOI       0x02  ///< Automatic EOI (not recommended).
#define ICW4_BUF_SECONDARY  0x08  ///< Buffered mode, secondary.
#define ICW4_BUF_PRIMARY    0x0C  ///< Buffered mode, primary.
#define ICW4_SFNM           0x10  ///< Special fully nested mode.
/// @}

/// @name Spurious IRQ lines
/// @{
#define PIC_SPURIOUS_IRQ1   7  ///< PIC1 spurious IRQ (hardware line 7).
#define PIC_SPURIOUS_IRQ2  15  ///< PIC2 spurious IRQ (hardware line 15).
/// @}

/// @name Initialization
/// @{

/**
 * @brief Initialize and remap the dual 8259 PIC.
 *
 * Performs the full ICW1-ICW4 initialization sequence on both PICs,
 * remapping IRQ 0-7 to vectors 32-39 and IRQ 8-15 to vectors 40-47.
 * All IRQs are masked (disabled) after initialization; use
 * @c pic_clear_mask() to enable specific lines.
 */
void pic_init(void);

/**
 * @brief Disable the PIC by masking all IRQ lines.
 *
 * Masks all 16 IRQ lines. Call this before enabling the APIC to
 * prevent both controllers from delivering interrupts simultaneously.
 */
void pic_disable(void);
/// @}

/// @name Interrupt handling
/// @{

/**
 * @brief Send End of Interrupt to the appropriate PIC(s).
 *
 * @param irq  IRQ number (0-15).
 *
 * For IRQs 0-7, sends EOI to PIC1 only. For IRQs 8-15, sends EOI
 * to both PIC2 and PIC1 (cascade).
 *
 * Must be called at the end of every IRQ handler to allow further
 * interrupts on this line and lower-priority lines.
 */
void pic_send_eoi(uint8_t irq);

/**
 * @brief Check whether an IRQ is a spurious interrupt.
 *
 * @param irq  IRQ number (only 7 and 15 can be spurious).
 *
 * Reads the In-Service Register to determine if the IRQ is genuine.
 * If spurious:
 * - IRQ 7:  caller must @b not send any EOI.
 * - IRQ 15: this function sends EOI to PIC1 internally; caller
 *           must @b not send a normal EOI.
 *
 * @return @c true if the IRQ is spurious (caller should skip
 *         processing and not send a normal EOI).
 */
bool pic_is_spurious(uint8_t irq);
/// @}

/// @name IRQ masking
/// @{

/// Disable (mask) a specific IRQ line.
void pic_set_mask(uint8_t irq);

/// Enable (unmask) a specific IRQ line.
void pic_clear_mask(uint8_t irq);
/// @}

/// @name Status queries
/// @{

/**
 * @brief Read the Interrupt Request Register (pending IRQs).
 * @return 16-bit bitmask: bit N = IRQ N has a pending request.
 *         Bits 0-7 from PIC1, bits 8-15 from PIC2.
 */
uint16_t pic_get_irr(void);

/**
 * @brief Read the In-Service Register (IRQs being handled).
 * @return 16-bit bitmask: bit N = IRQ N handler is executing.
 *         Bits 0-7 from PIC1, bits 8-15 from PIC2.
 */
uint16_t pic_get_isr(void);
/// @}

#endif // EMMK_PIC_H

