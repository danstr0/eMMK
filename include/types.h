/**
 * @file types.h
 * @brief Fundamental types, constants, and utility macros.
 *
 * @par Features
 * - Exact-width integer types (freestanding replacement for @c \<stdint.h\>)
 * - Pointer-sized and size types
 * - Boolean type and constants
 * - NULL pointer constant
 * - Common utility macros (alignment, min/max, bit manipulation)
 * - Compiler attribute shorthands
 * - Memory barriers and volatile access helpers
 *
 * @par Assumptions
 * - x86-64 LP64 data model (8-byte pointers, 4-byte int)
 * - GCC compiler (for @c __attribute__, @c __typeof__, @c __builtin_*)
 * - Freestanding environment (no standard library headers available)
 */

#ifndef EMMK_TYPES_H
#define EMMK_TYPES_H

/// @name Exact-width integer types
/// @{
typedef signed char         int8_t;
typedef unsigned char       uint8_t;
typedef signed short        int16_t;
typedef unsigned short      uint16_t;
typedef signed int          int32_t;
typedef unsigned int        uint32_t;
typedef signed long         int64_t;
typedef unsigned long       uint64_t;
/// @}

/// @name Pointer-sized and size types
/// @{
typedef int64_t   intptr_t;
typedef uint64_t  uintptr_t;
typedef int64_t   ptrdiff_t;
typedef uint64_t  size_t;
typedef int64_t   ssize_t;
/// @}

/// @name Boolean
/// @{
typedef _Bool  bool;
#define true   1
#define false  0
/// @}

/// NULL pointer constant.
#define NULL  ((void *)0)

/// @name Static assertions
/// @{
#define STATIC_ASSERT(cond, msg)  _Static_assert(cond, msg)

// Verify fundamental type size assumptions (LP64 / x86-64)
STATIC_ASSERT(sizeof(uint8_t)   == 1, "uint8_t must be 1 byte");
STATIC_ASSERT(sizeof(uint16_t)  == 2, "uint16_t must be 2 bytes");
STATIC_ASSERT(sizeof(uint32_t)  == 4, "uint32_t must be 4 bytes");
STATIC_ASSERT(sizeof(uint64_t)  == 8, "uint64_t must be 8 bytes");
STATIC_ASSERT(sizeof(uintptr_t) == 8, "uintptr_t must be pointer-sized (8 bytes)");
STATIC_ASSERT(sizeof(size_t)    == 8, "size_t must be 8 bytes on x86-64");
/// @}

/// @name Utility macros
/// @{

/// Number of elements in a statically-allocated array.
#define ARRAY_SIZE(x)  (sizeof(x) / sizeof((x)[0]))

/**
 * @brief Round @p x up to the nearest multiple of @p a.
 * @pre @p a must be a power of two.
 */
#define ALIGN_UP(x, a)    (((x) + ((a) - 1)) & ~((a) - 1))

/**
 * @brief Round @p x down to the nearest multiple of @p a.
 * @pre @p a must be a power of two.
 */
#define ALIGN_DOWN(x, a)  ((x) & ~((a) - 1))

/**
 * @brief Type-safe minimum.
 * 
 * Uses a GCC statement expression to evaluate each argument exactly once.
 *
 * @note Both operands are independently typed; mixing signed and unsigned
 *       types may trigger implicit conversions.
 */
#define MIN(a, b)           \
({                          \
    __typeof__(a) _a = (a); \
    __typeof__(b) _b = (b); \
    _a < _b ? _a : _b;      \
})

/**
 * @brief Type-safe maximum.
 * @copydetails MIN
 */
#define MAX(a, b)           \
({                          \
    __typeof__(a) _a = (a); \
    __typeof__(b) _b = (b); \
    _a > _b ? _a : _b;      \
})

/// Restrict @p val to the range [@p lo, @p hi].
#define CLAMP(val, lo, hi)  MIN(MAX(val, lo), hi)
/// @}

/// @name Struct introspection
/// @{

/// Byte offset of @p member within @p type.
#define offsetof(type, member)  __builtin_offsetof(type, member)

/**
 * @brief Cast a pointer-to-member back to the containing structure.
 *
 * @param ptr     Pointer to the member.
 * @param type    Type of the containing structure.
 * @param member  Name of the member within the structure.
 * @return Pointer to the enclosing @p type instance.
 */
#define container_of(ptr, type, member)                   \
({                                                        \
    const __typeof__(((type *)0)->member) *_mptr = (ptr); \
    (type *)((char *)_mptr - offsetof(type, member));     \
})
/// @}

/// @name Bit manipulation
/// @{
#define BIT(n)            (1UL << (n))
#define SET_BIT(x, n)     ((x) |= BIT(n))
#define CLEAR_BIT(x, n)   ((x) &= ~BIT(n))
#define TEST_BIT(x, n)    ((x) & BIT(n))
#define TOGGLE_BIT(x, n)  ((x) ^= BIT(n))
/// @}

/// @name Compiler attributes
/// @{
#define PACKED       __attribute__((packed))
#define ALIGNED(n)   __attribute__((aligned(n)))
#define NORETURN     __attribute__((noreturn))
#define UNUSED       __attribute__((unused))
#define LIKELY(x)    __builtin_expect(!!(x), 1)
#define UNLIKELY(x)  __builtin_expect(!!(x), 0)

/// Mark a function as printf-like for compile-time format-string checking.
#define PRINTF_FMT(fmt, args)  __attribute__((format(printf, fmt, args)))
/// @}

/// @name Memory barriers and volatile access
/// @{

/// Compiler memory barrier - prevents reordering across this point.
#define barrier()    __asm__ __volatile__("" ::: "memory")

/// CPU relaxation hint for spin-wait loops (x86 PAUSE instruction).
#define cpu_relax()  __asm__ __volatile__("pause" ::: "memory")

/**
 * @brief Read a value through a volatile pointer.
 *
 * Prevents the compiler from caching, reordering, or eliding the load.
 * Suitable for MMIO registers and shared-memory locations.
 */
#define READ_ONCE(x)                          \
({                                            \
    __typeof__(x) _val;                       \
    barrier();                                \
    _val = *((volatile __typeof__(x) *)&(x)); \
    barrier();                                \
    _val;                                     \
})

/**
 * @brief Write a value through a volatile pointer.
 *
 * Prevents the compiler from caching, reordering, or eliding the store.
 * Suitable for MMIO registers and shared-memory locations.
 */
#define WRITE_ONCE(x, val)                     \
do {                                           \
    barrier();                                 \
    *((volatile __typeof__(x) *)&(x)) = (val); \
    barrier();                                 \
} while (0)
/// @}

#endif // EMMK_TYPES_H

