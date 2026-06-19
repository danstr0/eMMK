/**
 * @file pic.c
 * @brief 8259 PIC driver.
 *
 * Implements the full initialization sequence (ICW1–ICW4), IRQ remapping,
 * EOI signaling, masking, and spurious interrupt detection for the
 * legacy dual 8259A PIC.
 *
 * After @c pic_init(), all 16 IRQ lines are masked. Drivers must call
 * @c pic_clear_mask() to enable specific IRQs as they register handlers.
 */

#include "pic.h"
#include "io.h"
#include "serial.h"

// ── Remapped vector offsets ────────────────────────────────────────────

/// PIC1 IRQs 0-7 are remapped to vectors 32-39.
#define PIC1_OFFSET  32

/// PIC2 IRQs 8-15 are remapped to vectors 40-47.
#define PIC2_OFFSET  40

// ── ICW3 cascade configuration ─────────────────────────────────────────

/**
 * Bitmask indicating which IRQ line the secondary is on.
 * Bit 2 set = secondary PIC is connected to IRQ 2.
 */
#define PIC1_ICW3_CASCADE  0x04

/**
 * Cascade identity — the IRQ line number on the primary that the
 * secondary is connected to.
 */
#define PIC2_ICW3_CASCADE  0x02

// ── Initialization ─────────────────────────────────────────────────────

void pic_init(void)
{
    kprintf("[PIC] Initializing 8259 PIC...\n");

    // ICW1: Begin initialization sequence
    // Both PICs must receive ICW1 simultaneously before any ICW2-ICW4
    // ICW1_INIT starts the sequence; ICW1_ICW4 signals that ICW4 follows
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    // ICW2: Set the vector offset for each PIC
    // This remaps IRQs away from the exception vector range (0-31)
    outb(PIC1_DATA, PIC1_OFFSET);
    io_wait();
    outb(PIC2_DATA, PIC2_OFFSET);
    io_wait();

    // ICW3: Configure the cascade connection
    // PIC1: bit 2 = secondary PIC on IRQ 2
    // PIC2: identity = 2 (matches the IRQ line on PIC1)
    outb(PIC1_DATA, PIC1_ICW3_CASCADE);
    io_wait();
    outb(PIC2_DATA, PIC2_ICW3_CASCADE);
    io_wait();

    // ICW4: Set operating mode
    // 8086 mode is required for x86 long mode operation
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    // Mask all IRQ lines. Drivers will unmask individual lines as
    // they register handlers. This prevents spurious interrupts from
    // firing before the system is ready.
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);

    kprintf("[PIC] Remapped: IRQ 0-7 -> vectors %u-%u, "
            "IRQ 8-15 -> vectors %u-%u\n",
            PIC1_OFFSET, PIC1_OFFSET + 7,
            PIC2_OFFSET, PIC2_OFFSET + 7);
    kprintf("[PIC] All IRQ lines masked\n");
}

void pic_disable(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
    kprintf("[PIC] Disabled (all IRQs masked)\n");
}

// ── End of interrupt ───────────────────────────────────────────────────

void pic_send_eoi(uint8_t irq)
{
    // For IRQs on the secondary PIC (8-15), EOI must be sent to both
    // PIC2 and PIC1 (because of the cascade)
    if (irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}

// ── Spurious interrupt detection ───────────────────────────────────────

bool pic_is_spurious(uint8_t irq)
{
    // A spurious interrupt occurs when the PIC's IRQ line is deasserted
    // between the PIC raising INT and the CPU acknowledging it
    if (irq == PIC_SPURIOUS_IRQ1)
    {
        outb(PIC1_COMMAND, PIC_READ_ISR);
        uint8_t isr = inb(PIC1_COMMAND);
        if (!(isr & (1 << 7)))  // spurious IRQ 7 - do not send any EOI
            return true;
    }
    else if (irq == PIC_SPURIOUS_IRQ2)
    {
        outb(PIC2_COMMAND, PIC_READ_ISR);
        uint8_t isr = inb(PIC2_COMMAND);
        if (!(isr & (1 << 7)))
        {
            // spurious IRQ 15 - do not send EOI to PIC2, but do send
            // EOI to PIC1 (PIC1 sees a real IRQ 2 from the cascade)
            outb(PIC1_COMMAND, PIC_EOI);
            return true;
        }
    }

    return false;
}

// ── IRQ masking ────────────────────────────────────────────────────────

void pic_set_mask(uint8_t irq)
{
    uint16_t port;
    uint8_t  bit = irq;

    if (bit < 8)
        port = PIC1_DATA;
    else
    {
        port = PIC2_DATA;
        bit -= 8;
    }

    outb(port, inb(port) | (1 << bit));
}

void pic_clear_mask(uint8_t irq)
{
    uint16_t port;
    uint8_t  bit = irq;

    if (bit < 8)
        port = PIC1_DATA;
    else
    {
        port = PIC2_DATA;
        bit -= 8;
    }

    outb(port, inb(port) & ~(1 << bit));

    // If unmasking a secondary PIC line (IRQ 8-15), also ensure the
    // cascade line (IRQ 2 on PIC1) is unmasked - otherwise PIC2
    // interrupts will never reach the CPU
    if (port == PIC2_DATA)
    {
        uint8_t mask1 = inb(PIC1_DATA);
        if (mask1 & (1 << 2))
            outb(PIC1_DATA, mask1 & ~(1 << 2));
    }
}

// ── Status queries ─────────────────────────────────────────────────────

/**
 * @brief Read a PIC register (IRR or ISR) from both PICs.
 * @param ocw3  Register select command (@c PIC_READ_IRR or
 *              @c PIC_READ_ISR).
 * @return Combined 16-bit value (PIC2 in high byte, PIC1 in low byte).
 */
static uint16_t pic_get_reg(uint8_t ocw3)
{
    outb(PIC1_COMMAND, ocw3);
    outb(PIC2_COMMAND, ocw3);
    return ((uint16_t)inb(PIC2_COMMAND) << 8) | inb(PIC1_COMMAND);
}

uint16_t pic_get_irr(void)
{
    return pic_get_reg(PIC_READ_IRR);
}

uint16_t pic_get_isr(void)
{
    return pic_get_reg(PIC_READ_ISR);
}

