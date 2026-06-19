/**
 * @file io.h
 * @brief Port I/O and memory-mapped I/O primitives.
 *
 * @par Features
 * - Scalar port I/O:  @c inb / @c inw / @c inl, @c outb / @c outw / @c outl
 * - String port I/O:  @c insl, @c outsl (REP-prefixed bulk transfers)
 * - I/O delay:        @c io_wait (for legacy devices requiring bus settling)
 * - MMIO accessors:   @c mmio_read{8,16,32,64}, @c mmio_write{8,16,32,64}
 *
 * All functions include a compiler memory barrier (@c "memory" clobber or
 * explicit @c barrier()) to prevent reordering of I/O with respect to 
 * ordinary memory accesses.
 */

#ifndef EMMK_IO_H
#define EMMK_IO_H

#include "types.h"

/// @name Scalar port I/O
/// @{
static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}
/// @}

/// @name String port I/O
/// @{

/**
 * @brief Read @p count 32-bit dwords from @p port into @p addr.
 *
 * @param port   I/O port to read from.
 * @param addr   Destination buffer (should be dword-aligned).
 * @param count  Number of 32-bit dwords to read.
 *
 * @pre @p addr must be writable for at least @c 4 * @p count bytes.
 */
static inline void insl(uint16_t port, void *addr, size_t count)
{
    __asm__ volatile("rep insl"
                     : "+D"(addr), "+c"(count)
                     : "d"(port)
                     : "memory");
}

/**
 * @brief Write @p count 32-bit dwords from @p addr to @p port.
 *
 * @param port   I/O port to write to.
 * @param addr   Source buffer (should be dword-aligned).
 * @param count  Number of 32-bit dwords to write.
 *
 * @pre @p addr must be readable for at least @c 4 * @p count bytes.
 */
static inline void outsl(uint16_t port, const void *addr, size_t count)
{
    __asm__ volatile("rep outsl"
                     : "+S"(addr), "+c"(count)
                     : "d"(port)
                     : "memory");
}
/// @}

/**
 * @brief Insert a small delay (~1 µs) for legacy device settling.
 *
 * Writes to port @c 0x80 (POST code diagnostic port), which is a safe
 * no-op on all modern hardware.
 */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

/// @name MMIO read accessors
/// @brief MMIO read accessors.
///
/// Each access is performed through a @c volatile pointer and bracketed
/// by compiler barriers to prevent reordering with adjacent memory or
/// I/O operations.
/// @{

static inline uint8_t mmio_read8(volatile void* addr)
{
    uint8_t val;
    barrier();
    val = *(volatile uint8_t *)addr;
    barrier();
    return val;
}

static inline uint16_t mmio_read16(volatile void* addr)
{
    uint16_t val;
    barrier();
    val = *(volatile uint16_t *)addr;
    barrier();
    return val;
}

static inline uint32_t mmio_read32(volatile void* addr)
{
    uint32_t val;
    barrier();
    val = *(volatile uint32_t *)addr;
    barrier();
    return val;
}

static inline uint64_t mmio_read64(volatile void* addr)
{
    uint64_t val;
    barrier();
    val = *(volatile uint64_t *)addr;
    barrier();
    return val;
}
/// @}

/// @name MMIO write accessors
///
/// Each access is performed through a @c volatile pointer and bracketed
/// by compiler barriers to prevent reordering with adjacent memory or
/// I/O operations.
/// @{

static inline void mmio_write8(volatile void* addr, uint8_t value)
{
    barrier();
    *(volatile uint8_t *)addr = value;
    barrier();
}

static inline void mmio_write16(volatile void* addr, uint16_t value)
{
    barrier();
    *(volatile uint16_t *)addr = value;
    barrier();
}

static inline void mmio_write32(volatile void* addr, uint32_t value)
{
    barrier();
    *(volatile uint32_t *)addr = value;
    barrier();
}

static inline void mmio_write64(volatile void* addr, uint64_t value)
{
    barrier();
    *(volatile uint64_t *)addr = value;
    barrier();
}
/// @}

#endif // EMMK_IO_H

