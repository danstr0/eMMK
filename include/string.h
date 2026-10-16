/**
 * @file string.h
 * @brief Freestanding string and memory manipulation functions.
 *
 * Provides standard C library memory and string operations implemented
 * without any external dependencies. All functions follow their POSIX / C
 * standard semantics unless otherwise noted.
 */

#ifndef EMMK_STRING_H
#define EMMK_STRING_H

#include "types.h"

/// @name Memory operations
/// @{

/// Fill @p n bytes of @p dest with the byte value @p c. Returns @p dest.
void *memset(void *dest, int c, size_t n);

/// Copy @p n bytes from @p src to @p dest (regions must not overlap).
/// Returns @p dest.
void *memcpy(void *dest, const void *src, size_t n);

/// Copy @p n bytes from @p src to @p dest (safe for overlapping regions).
/// Returns @p dest.
void *memmove(void *dest, const void *src, size_t n);

/// Compare @p n bytes of @p s1 and @p s2. Returns <0, 0, or >0.
int memcmp(const void *s1, const void *s2, size_t n);

/// Find the first occurrence of byte @p c in the first @p n bytes of @p s.
/// Returns @c NULL if not found.
void *memchr(const void *s, int c, size_t n);

/**
 * @brief Route the core memory functions through GCC builtins.
 *
 * @c -ffreestanding implies @c -fno-builtin, so without these mappings
 * every call is an out-of-line call. Through @c __builtin_*, GCC expands
 * small constant-size operations inline and still emits a call to the
 * real function otherwise.
 */
#ifndef EMMK_STRING_IMPL
#define memset(dest, c, n)    __builtin_memset((dest), (c), (n))
#define memcpy(dest, src, n)  __builtin_memcpy((dest), (src), (n))
#define memmove(dest, src, n) __builtin_memmove((dest), (src), (n))
#define memcmp(s1, s2, n)     __builtin_memcmp((s1), (s2), (n))
#endif

/// Zero @p n bytes starting at @p dest.
static inline void memzero(void *dest, size_t n)
{
    memset(dest, 0, n);
}
/// @}

/// @name String examination
/// @{

/// Return the length of @p s (not counting the null terminator).
size_t strlen(const char *s);

/// Return the length of @p s, but at most @p maxlen.
size_t strnlen(const char *s, size_t maxlen);

/// Compare two strings lexicographically. Returns <0, 0, or >0.
int strcmp(const char *s1, const char *s2);

/// Compare at most @p n characters of two strings. Returns <0, 0, or >0.
int strncmp(const char *s1, const char *s2, size_t n);

/// Find the first occurrence of @p c in @p s. Returns @c NULL if not found.
char *strchr(const char *s, int c);

/// Find the last occurrence of @p c in @p s. Returns @c NULL if not found.
char *strrchr(const char *s, int c);

/// Find the first occurrence of @p needle in @p haystack.
/// Returns @c NULL if not found.
char *strstr(const char *haystack, const char *needle);
/// @}

/// @name Safe string copy / concatenation
/// @{

/**
 * @brief Copy a string with guaranteed null termination.
 *
 * @param dest  Destination buffer.
 * @param src   Source string.
 * @param size  Total size of @p dest (including space for '\\0').
 * @return Length of @p src (allows truncation detection:
 *         if the return value ≥ @p size, truncation occurred).
 */
size_t strlcpy(char *dest, const char *src, size_t size);

/**
 * @brief Concatenate string with guaranteed null termination.
 *
 * @param dest  Destination buffer (must already be null-terminated).
 * @param src   Source string to append.
 * @param size  Total size of @p dest.
 * @return <code>strlen(src) + min(size, strlen(dest))</code>; if the
 *         return value ≥ @p size, truncation occurred.
 */
size_t strlcat(char *dest, const char *src, size_t size);
/// @}

#endif // EMMK_STRING_H

