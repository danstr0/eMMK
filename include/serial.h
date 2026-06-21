/**
 * @file serial.h
 * @brief UART serial port driver interface (8250/16550A compatible).
 *
 * Provides initialization, polled output, and formatted printing over
 * standard PC serial ports. COM1 is used as the default kernel console 
 * via the @c kprintf / @c kputs / @c kputchar convenience macros.
 *
 * @note Output-only (polled TX).
 *
 * @note @c serial_puts() and @c serial_printf() are internally synchronized
 *       and disable local interrupts while transmitting. @c serial_putchar()
 *       performs no synchronization and should only be used when external
 *       serialization is already guaranteed.
 */

#ifndef EMMK_SERIAL_H
#define EMMK_SERIAL_H

#include "types.h"

/// @name Standard serial port base addresses
/// @{
#define COM1_PORT  0x3F8
#define COM2_PORT  0x2F8
#define COM3_PORT  0x3E8
#define COM4_PORT  0x2E8
/// @}

/// @name UART register offsets (from port base address)
/// @{

// General registers (DLAB = 0)
#define UART_DATA          0  ///< TX/RX data register.
#define UART_INT_ENABLE    1  ///< Interrupt enable register.
#define UART_FIFO_CTRL     2  ///< FIFO control register (write-only).
#define UART_LINE_CTRL     3  ///< Line control register.
#define UART_MODEM_CTRL    4  ///< Modem control register.
#define UART_LINE_STATUS   5  ///< Line status register  (read-only).
#define UART_MODEM_STATUS  6  ///< Modem status register (read-only).
#define UART_SCRATCH       7  ///< Scratch register.

// Divisor latch registers (DLAB = 1)
#define UART_DIVISOR_LOW   0  ///< Divisor latch low byte.
#define UART_DIVISOR_HIGH  1  ///< Divisor latch high byte.
/// @}

/// @name Line Control Register bits
/// @{

// Word length - bits [1:0]
#define UART_LCR_WORD_5        0x00  ///< 5 data bits.
#define UART_LCR_WORD_6        0x01  ///< 6 data bits.
#define UART_LCR_WORD_7        0x02  ///< 7 data bits.
#define UART_LCR_WORD_8        0x03  ///< 8 data bits.

// Stop bits - bit [2]
#define UART_LCR_STOP_1        0x00  ///< 1 stop bit.
#define UART_LCR_STOP_2        0x04  ///< 2 stop bits.

// Parity - bits [5:3]
#define UART_LCR_PARITY_NONE   0x00  ///< No parity.
#define UART_LCR_PARITY_ODD    0x08  ///< Odd parity.
#define UART_LCR_PARITY_EVEN   0x18  ///< Even parity.
#define UART_LCR_PARITY_MARK   0x28  ///< Mark (always 1).
#define UART_LCR_PARITY_SPACE  0x38  ///< Space (always 0).

// DLAB - bit [7]
#define UART_LCR_DLAB          0x80  ///< Enable access to divisor latch registers.
/// @}

/// @name FIFO Control Register bits
/// @{
#define UART_FCR_ENABLE      0x01  ///< Enable FIFOs.
#define UART_FCR_CLEAR_RX    0x02  ///< Clear receive FIFO.
#define UART_FCR_CLEAR_TX    0x04  ///< Clear transmit FIFO.
#define UART_FCR_DMA_MODE    0x08  ///< DMA mode select.

// Receive FIFO trigger level - bits [7:6]
#define UART_FCR_TRIGGER_1   0x00  ///< Trigger at 1 byte.
#define UART_FCR_TRIGGER_4   0x40  ///< Trigger at 4 bytes.
#define UART_FCR_TRIGGER_8   0x80  ///< Trigger at 8 bytes.
#define UART_FCR_TRIGGER_14  0xC0  ///< Trigger at 14 bytes.
/// @}

/// @name Modem Control Register bits
/// @{
#define UART_MCR_DTR       0x01  ///< Data Terminal Ready.
#define UART_MCR_RTS       0x02  ///< Request To Send.
#define UART_MCR_OUT1      0x04  ///< Auxiliary output 1.
#define UART_MCR_OUT2      0x08  ///< Auxiliary output 2.
#define UART_MCR_LOOPBACK  0x10  ///< Loopback mode.
/// @}

/// @name Line Status Register bits
/// @{
#define UART_LSR_DATA_READY   0x01  ///< Data available to read.
#define UART_LSR_OVERRUN_ERR  0x02  ///< Overrun error.
#define UART_LSR_PARITY_ERR   0x04  ///< Parity error.
#define UART_LSR_FRAMING_ERR  0x08  ///< Framing error.
#define UART_LSR_BREAK_INT    0x10  ///< Break indicator.
#define UART_LSR_TX_EMPTY     0x20  ///< Transmitter holding register empty.
#define UART_LSR_TX_IDLE      0x40  ///< Transmitter idle.
#define UART_LSR_FIFO_ERR     0x80  ///< Error in receive FIFO.
/// @}

/// @name Interrupt Enable Register bits
/// @{
#define UART_IER_RX_AVAIL      0x01  ///< Received data available.
#define UART_IER_TX_EMPTY      0x02  ///< Transmitter holding register empty.
#define UART_IER_LINE_STATUS   0x04  ///< Receiver line status change.
#define UART_IER_MODEM_STATUS  0x08  ///< Modem status change.
/// @}

/// Standard UART base clock rate (1.8432 MHz ÷ 16 = 115200 Hz).
#define UART_BASE_CLOCK  115200

/// @name API
/// @{

/**
 * @brief Initialize a serial port for 8N1 communication.
 *
 * Configures the port for 8 data bits, no parity, 1 stop bit with FIFOs
 * enabled and interrupts disabled.
 *
 * @param port  I/O base address (e.g., @c COM1_PORT).
 * @param baud  Desired baud rate (e.g., 9600, 115200).
 */
void serial_init(uint16_t port, uint32_t baud);

/**
 * @brief Write a single character (blocking / polled).
 *
 * Spins until the transmit holding register is empty, then writes @p c.
 *
 * @param port  I/O base address.
 * @param c     Character to transmit.
 *
 * @note This function performs no synchronization.
 */
void serial_putchar(uint16_t port, char c);

/**
 * @brief Write a null-terminated string.
 *
 * Output is internally serialized and safe to call concurrently from
 * multiple CPUs and interrupt contexts.
 *
 * @param port  I/O base address.
 * @param s     String to transmit.
 */
void serial_puts(uint16_t port, const char *s);

/**
 * @brief Write formatted output to a serial port.
 *
 * Supports a subset of standard printf format specifiers.
 *
 * Output is internally serialized and safe to call concurrently from
 * multiple CPUs and interrupt contexts.
 *
 * @param port  I/O base address.
 * @param fmt   printf-style format string.
 * @param ...   Format arguments.
 */
PRINTF_FMT(2, 3)
void serial_printf(uint16_t port, const char *fmt, ...);
/// @}

/// @name Kernel console convenience macros
/// @{

/// Formatted print to the kernel console (COM1).
#define kprintf(fmt, ...)  serial_printf(COM1_PORT, fmt, ##__VA_ARGS__)

/// Write a string to the kernel console (COM1).
#define kputs(s)           serial_puts(COM1_PORT, s)

/// Write a single character to the kernel console (COM1).
/// @note Not internally synchronized.
#define kputchar(c)        serial_putchar(COM1_PORT, c)
/// @}

#endif // EMMK_SERIAL_H

