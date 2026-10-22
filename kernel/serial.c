/**
 * @file serial.c
 * @brief UART serial port driver (8250/16550A compatible).
 *
 * Implements polled (blocking) serial output: raw character output, string
 * output with automatic CR/LF translation, and formatted output via the
 * shared formatter in format.c (see format.h for supported specifiers).
 *
 * @note All output paths insert @c \\r before every @c \\n. If a string
 *       already contains @c \\r\\n sequences, the result will be
 *       @c \\r\\r\\n.
 *
 * @warning Output is synchronous: the caller spins on the UART with
 *          interrupts disabled for the whole message. At 115200 baud
 *          that is ~87 µs per character on real hardware. Never print
 *          from a latency-sensitive path.
 */

#include "serial.h"
#include "format.h"
#include "io.h"
#include "spinlock.h"

static spinlock_t serial_lock = SPINLOCK_INIT;

/// Set once by @c serial_panic_enter(); never cleared.
static volatile bool serial_panic_mode = false;

// ── Initialization ─────────────────────────────────────────────────────

void serial_init(uint16_t port, uint32_t baud)
{
    if (baud == 0 || baud > UART_BASE_CLOCK)
        baud = UART_BASE_CLOCK;

    uint16_t divisor = UART_BASE_CLOCK / baud;

    // 1. Disable all interrupts
    outb(port + UART_INT_ENABLE, 0x00);

    // 2. Set baud rate via the divisor latch
    outb(port + UART_LINE_CTRL, UART_LCR_DLAB);
    outb(port + UART_DIVISOR_LOW,  (uint8_t)(divisor & 0xFF));
    outb(port + UART_DIVISOR_HIGH, (uint8_t)((divisor >> 8) & 0xFF));

    // 3. Configure 8 data bits, no parity, 1 stop bit (8N1)
    outb(port + UART_LINE_CTRL,
         UART_LCR_WORD_8 | UART_LCR_PARITY_NONE | UART_LCR_STOP_1);

    // 4. Enable and clear FIFOs with a 14-byte trigger threshold
    outb(port + UART_FIFO_CTRL,
         UART_FCR_ENABLE | UART_FCR_CLEAR_RX
         | UART_FCR_CLEAR_TX | UART_FCR_TRIGGER_14);

    // 5. Assert DTR + RTS and enable the IRQ line via OUT2
    outb(port + UART_MODEM_CTRL,
         UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT2);
}

// ── Low-level output ───────────────────────────────────────────────────

/**
 * @brief Test whether the transmit holding register is empty.
 * @param port  UART base address.
 * @return @c true if a byte can be written immediately.
 */
static inline bool serial_is_transmit_empty(uint16_t port)
{
    return (inb(port + UART_LINE_STATUS) & UART_LSR_TX_EMPTY) != 0;
}

void serial_putchar(uint16_t port, char c)
{
    while (!serial_is_transmit_empty(port))
        cpu_relax();
    outb(port + UART_DATA, c);
}

/// Write one character, translating @c \\n to @c \\r\\n.
static void serial_put_translated(uint16_t port, char c)
{
    if (c == '\n')
        serial_putchar(port, '\r');
    serial_putchar(port, c);
}

// ── Console lock ───────────────────────────────────────────────────────

/**
 * @brief Disable interrupts and take the console lock.
 *
 * In panic mode the lock is skipped: the panicking core may itself hold
 * it (a fault inside @c kprintf), and waiting would deadlock.
 *
 * @return @c true if the lock was taken (pass to @c console_unlock).
 */
static bool console_lock(uint64_t *flags)
{
    *flags = irq_save();
    if (serial_panic_mode)
        return false;
    spin_lock(&serial_lock);
    return true;
}

static void console_unlock(bool locked, uint64_t flags)
{
    if (locked)
        spin_unlock(&serial_lock);
    irq_restore(flags);
}

void serial_panic_enter(void)
{
    serial_panic_mode = true;
}

// ── String / formatted output ──────────────────────────────────────────

void serial_puts(uint16_t port, const char *s)
{
    uint64_t flags;
    bool locked = console_lock(&flags);

    while (*s)
        serial_put_translated(port, *s++);

    console_unlock(locked, flags);
}

/// @c kvformat sink: @p ctx carries the port number.
static void serial_format_sink(void *ctx, char c)
{
    serial_put_translated((uint16_t)(uintptr_t)ctx, c);
}

void serial_vprintf(uint16_t port, const char *fmt, __builtin_va_list args)
{
    uint64_t flags;
    bool locked = console_lock(&flags);

    kvformat(serial_format_sink, (void *)(uintptr_t)port, fmt, args);

    console_unlock(locked, flags);
}

void serial_printf(uint16_t port, const char *fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    serial_vprintf(port, fmt, args);
    __builtin_va_end(args);
}

