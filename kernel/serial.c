/**
 * @file serial.c
 * @brief UART serial port driver (8250/16550A compatible).
 *
 * Implements polled (blocking) serial output: raw character output, string
 * output with automatic CR/LF translation, and a minimal @c printf.
 *
 * @par Supported format specifiers
 * - @c \%d, @ \%i : signed decimal (@c int; @c int64_t with @c l / @c ll)
 * - @c \%u        : unsigned decimal (@c unsigned; @c uint64_t with @c l / @c ll)
 * - @c \%x        : unsigned hexadecimal, lowercase
 * - @c \%X        : unsigned hexadecimal, uppercase
 * - @c \%p        : pointer (printed as @c 0x..., lowercase hex)
 * - @c \%s        : null-terminated string (@c NULL prints "(null)")
 * - @c \%c        : single character
 * - @c \%\%       : literal percent sign
 *
 * @par Length modifiers
 * - (none) : @c int / @c unsigned (32-bit after default promotion)
 * - @cl    : @c long (64-bit on x86-64 LP64)
 * - @c ll  : <code>long long</code> (64-bit)
 *
 * @note @c serial_puts() and the literal-character path in @c serial_printf()
 *       both insert @c \\r before @c \\n. If a @c \%s argument already
 *       contains @c \\r\\n sequences, the result will be @c \\r\\r\\n.
 */

#include "serial.h"
#include "io.h"
#include "spinlock.h"

static spinlock_t serial_lock = SPINLOCK_INIT;

// ── Initialization ─────────────────────────────────────────────────────

void serial_init(uint16_t port, uint32_t baud)
{
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

/// Lock-free version of @c serial_puts, for use by @c serial_printf.
static void serial_puts_internal(uint16_t port, const char *s)
{
    while (*s)
    {
        if (*s == '\n')
            serial_putchar(port, '\r');
        serial_putchar(port, *s++);
    }
}

void serial_puts(uint16_t port, const char *s)
{
    uint64_t flags;
    spin_lock_irqsave(&serial_lock, &flags);

    serial_puts_internal(port, s);

    spin_unlock_irqrestore(&serial_lock, flags);
}

// ── printf internals ───────────────────────────────────────────────────

/**
 * @brief Print an unsigned integer in the given base.
 *
 * @param port       UART base address.
 * @param value      Value to print.
 * @param base       Radix (2-16).
 * @param uppercase  If @c true, use uppercase hex digits (A-F)
 */
static void serial_put_uint(uint16_t port, uint64_t value,
                            int base, bool uppercase)
{
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = uppercase ? digits_upper : digits_lower;

    // 65 chars covers a full 64-bit binary representation + NUL.
    char buf[65];
    int i = 0;

    if (value == 0)
    {
        serial_putchar(port, '0');
        return;
    }

    while (value > 0)
    {
        buf[i++] = digits[value % base];
        value /= base;
    }

    while (i > 0)
        serial_putchar(port, buf[--i]);
}

/**
 * @brief Print a signed decimal integer in the given base.
 *
 * Negative values are prefixed with '-'; the magnitude is printed
 * via @c serial_put_uint to avoid undefined behavior with @c INT64_MIN.
 */
static void serial_put_int(uint16_t port, int64_t value)
{
    if (value < 0)
    {
        serial_putchar(port, '-');
        serial_put_uint(port, -(uint64_t)value, 10, false);
    }
    else
        serial_put_uint(port, (uint64_t)value, 10, false);
}

// ── Formatted output ───────────────────────────────────────────────────

/// Length modifier state for printf parsing.
typedef enum
{
    LEN_NONE,       ///< @c int / @c unsigned
    LEN_LONG,       ///< @c long / <code>unsigned long</code> (64-bit on LP64)
    LEN_LONG_LONG,  ///< <code>long long</code> / <code>unsigned long long</code>
} printf_length_t;

void serial_printf(uint16_t port, const char *fmt, ...)
{
    uint64_t flags;
    spin_lock_irqsave(&serial_lock, &flags);

    __builtin_va_list args;
    __builtin_va_start(args, fmt);

    while (*fmt)
    {
        if (*fmt != '%')
        {
            if (*fmt == '\n')
                serial_putchar(port, '\r');
            serial_putchar(port, *fmt++);
            continue;
        }

        fmt++;  // skip '%'
        if (*fmt == '\0')
        {
            serial_putchar(port, '%');
            break;
        }

        // Parse length modifier
        printf_length_t length = LEN_NONE;
        if (*fmt == 'l')
        {
            fmt++;
            if (*fmt == 'l')
            {
                length = LEN_LONG_LONG;
                fmt++;
            }
            else
                length = LEN_LONG;
        }

        // Dispatch on conversion specifier
        switch (*fmt)
        {
        case 'd':
        case 'i':
        {
            int64_t val;
            if (length >= LEN_LONG)
                val = __builtin_va_arg(args, int64_t);
            else
                val = __builtin_va_arg(args, int);
            serial_put_int(port, val);
            break;
        }
        case 'u':
        {
            uint64_t val;
            if (length >= LEN_LONG)
                val = __builtin_va_arg(args, uint64_t);
            else
                val = __builtin_va_arg(args, unsigned int);
            serial_put_uint(port, val, 10, false);
            break;
        }
        case 'x':
        case 'X':
        {
            uint64_t val;
            if (length >= LEN_LONG)
                val = __builtin_va_arg(args, uint64_t);
            else
                val = __builtin_va_arg(args, unsigned int);
            serial_put_uint(port, val, 16, *fmt == 'X');
            break;
        }
        case 'p':
            serial_puts_internal(port, "0x");
            serial_put_uint(port,
                            (uintptr_t)__builtin_va_arg(args, void *), 
                            16, false);
            break;
        case 's':
        {
            const char *s = __builtin_va_arg(args, const char *);
            serial_puts_internal(port, s ? s : "(null)");
            break;
        }
        case 'c':
        {
            char ch = (char)__builtin_va_arg(args, int);
            if (ch == '\n')
                serial_putchar(port, '\r');
            serial_putchar(port, ch);
            break;
        }
        case '%':
            serial_putchar(port, '%');
            break;
        default:
            // Unknown specifier - print literally
            serial_putchar(port, '%');
            serial_putchar(port, *fmt);
            break;
        }

        fmt++;
    }

    __builtin_va_end(args);

    spin_unlock_irqrestore(&serial_lock, flags);
}

