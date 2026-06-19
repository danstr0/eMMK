/**
 * @file endian.h
 * @brief Byte-order conversion macros.
 *
 * Network protocols use big-endian (network byte order), while x86 is 
 * little-endian. These macros convert between the two.
 *
 * @par Naming conventions
 * - @c htons / @c htonl — host to network (standard POSIX names).
 * - @c ntohs / @c ntohl — network to host.
 * - @c cpu_to_be*       — CPU (little) to big-endian (same as hton*).
 * - @c be*_to_cpu       — big-endian to CPU (same as ntoh*).
 * - @c cpu_to_le*       — CPU to little-endian (identity on x86).
 * - @c le*_to_cpu       — little-endian to CPU (identity on x86).
 *
 * Implementation uses GCC @c __builtin_bswap* intrinsics, which compile
 * to single BSWAP instructions on x86.
 */

#ifndef EMMK_ENDIAN_H
#define EMMK_ENDIAN_H

#include "types.h"

/// @name Byte swap primitives
/// @{

static inline uint16_t bswap16(uint16_t x)
{
    return __builtin_bswap16(x);
}

static inline uint32_t bswap32(uint32_t x)
{
    return __builtin_bswap32(x);
}

static inline uint64_t bswap64(uint64_t x)
{
    return __builtin_bswap64(x);
}
/// @}

/// @name Host ↔ network byte order
/// @{

#define htons(x)   ((uint16_t)bswap16((uint16_t)(x)))  ///< Host to network, 16-bit.
#define htonl(x)   ((uint32_t)bswap32((uint32_t)(x)))  ///< Host to network, 32-bit.
#define htonll(x)  ((uint64_t)bswap64((uint64_t)(x)))  ///< Host to network, 64-bit.

#define ntohs(x)   ((uint16_t)bswap16((uint16_t)(x)))  ///< Network to host, 16-bit.
#define ntohl(x)   ((uint32_t)bswap32((uint32_t)(x)))  ///< Network to host, 32-bit.
#define ntohll(x)  ((uint64_t)bswap64((uint64_t)(x)))  ///< Network to host, 64-bit.
/// @}

/// @name CPU ↔ big-endian
/// @{
#define cpu_to_be16(x)  ((uint16_t)bswap16((uint16_t)(x)))
#define cpu_to_be32(x)  ((uint32_t)bswap32((uint32_t)(x)))
#define cpu_to_be64(x)  ((uint64_t)bswap64((uint64_t)(x)))

#define be16_to_cpu(x)  ((uint16_t)bswap16((uint16_t)(x)))
#define be32_to_cpu(x)  ((uint32_t)bswap32((uint32_t)(x)))
#define be64_to_cpu(x)  ((uint64_t)bswap64((uint64_t)(x)))
/// @}

/// @name CPU ↔ little-endian (identity on x86)
/// @{
#define cpu_to_le16(x)  ((uint16_t)(x))
#define cpu_to_le32(x)  ((uint32_t)(x))
#define cpu_to_le64(x)  ((uint64_t)(x))

#define le16_to_cpu(x)  ((uint16_t)(x))
#define le32_to_cpu(x)  ((uint32_t)(x))
#define le64_to_cpu(x)  ((uint64_t)(x))
/// @}

#endif // EMMK_ENDIAN_H

