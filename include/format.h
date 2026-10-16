/**
 * @file format.h
 * @brief printf-style formatting engine.
 *
 * The formatter writes characters through a caller-supplied sink
 * callback, so one implementation backs both the serial console
 * (@c kprintf) and in-memory formatting (@c ksnprintf).
 *
 * @par Supported conversions
 * @c d @c i @c u @c x @c X @c o @c p @c s @c c @c \%
 *
 * @par Supported modifiers
 * - Flags: @c - (left-justify), @c 0 (zero-pad), @c + (force sign),
 *   space (space for positive sign), @c # (@c 0x / @c 0 prefix for x / o)
 * - Width: decimal digits or @c * (taken from the argument list)
 * - Precision: @c .N or @c .* — minimum digits for integers, maximum
 *   characters for @c \%s
 * - Length: @c hh, @c h, @c l, @c ll, @c z, @c j, @c t
 *
 * Every specifier that GCC's @c -Wformat accepts for these conversions
 * consumes exactly the right argument, so a format string can never
 * desynchronize the argument list. Floating-point conversions are not
 * supported (the kernel is built without SSE); an unknown conversion is
 * printed literally.
 */

#ifndef EMMK_FORMAT_H
#define EMMK_FORMAT_H

#include "types.h"

/**
 * @brief Character sink for @c kvformat.
 *
 * @param ctx  Opaque context passed through from @c kvformat.
 * @param c    Next output character.
 */
typedef void (*format_sink_t)(void *ctx, char c);

/**
 * @brief Format @p fmt with @p args, emitting each character via @p sink.
 *
 * @param sink  Character output callback.
 * @param ctx   Opaque context forwarded to @p sink.
 * @param fmt   printf-style format string.
 * @param args  Argument list.
 * @return Number of characters emitted.
 */
size_t kvformat(format_sink_t sink, void *ctx,
                const char *fmt, __builtin_va_list args);

/**
 * @brief Format into a fixed-size buffer.
 *
 * Always null-terminates when @p size > 0.
 *
 * @param buf   Destination buffer.
 * @param size  Size of @p buf in bytes (including the terminator).
 * @param fmt   printf-style format string.
 * @return Length the formatted string would have had with unlimited space
 *         (excluding the terminator); a return value ≥ @p size means the
 *         output was truncated.
 */
size_t kvsnprintf(char *buf, size_t size, const char *fmt,
                  __builtin_va_list args);

/// @copydoc kvsnprintf
PRINTF_FMT(3, 4)
size_t ksnprintf(char *buf, size_t size, const char *fmt, ...);

#endif // EMMK_FORMAT_H

