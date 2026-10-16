/**
 * @file format.c
 * @brief printf-style formatting engine.
 */

#include "format.h"
#include "string.h"

// ── Specifier state ────────────────────────────────────────────────────

/// Argument width selected by the length modifier.
typedef enum
{
    LEN_INT,    ///< (none): @c int / @c unsigned
    LEN_CHAR,   ///< @c hh: promoted @c int, truncated to @c char
    LEN_SHORT,  ///< @c h:  promoted @c int, truncated to @c short
    LEN_LONG,   ///< @c l, @c ll, @c z, @c j, @c t: 64-bit on LP64
} fmt_length_t;

/// One parsed conversion specification.
struct fmt_spec
{
    bool         left;       ///< @c - : left-justify within the width.
    bool         zero;       ///< @c 0 : pad numbers with zeros.
    bool         plus;       ///< @c + : always print a sign.
    bool         space;      ///< ' '  : space in place of a '+' sign.
    bool         alt;        ///< @c # : alternate form (0x / leading 0).
    int          width;      ///< Minimum field width (0 = none).
    int          precision;  ///< -1 = not specified.
    fmt_length_t length;
};

/// Output state threaded through the emit helpers.
struct fmt_out
{
    format_sink_t sink;
    void         *ctx;
    size_t        count;  ///< Characters emitted so far.
};

// ── Emit helpers ───────────────────────────────────────────────────────

static void emit(struct fmt_out *out, char c)
{
    out->sink(out->ctx, c);
    out->count++;
}

static void emit_repeat(struct fmt_out *out, char c, int n)
{
    while (n-- > 0)
        emit(out, c);
}

/**
 * @brief Emit one padded field: `[pad][prefix][zeros][body][pad]`.
 *
 * @param numeric     Whether the @c 0 flag applies (numbers only, and
 *                    only when no precision was given).
 * @param prefix      Sign and/or radix prefix (may be empty).
 * @param body        Field contents (digits or string).
 * @param body_len    Length of @p body.
 * @param min_digits  Minimum body length, zero-filled (integer precision).
 */
static void emit_field(struct fmt_out *out, const struct fmt_spec *spec,
                       bool numeric, const char *prefix,
                       const char *body, int body_len, int min_digits)
{
    int prefix_len = (int)strlen(prefix);
    int zeros      = min_digits > body_len ? min_digits - body_len : 0;
    int total      = prefix_len + zeros + body_len;
    int pad        = spec->width > total ? spec->width - total : 0;

    bool zero_pad = numeric && spec->zero && !spec->left
                    && spec->precision < 0;

    if (!spec->left && !zero_pad)
        emit_repeat(out, ' ', pad);

    for (int i = 0; i < prefix_len; ++i)
        emit(out, prefix[i]);

    if (zero_pad)
        emit_repeat(out, '0', pad);

    emit_repeat(out, '0', zeros);

    for (int i = 0; i < body_len; ++i)
        emit(out, body[i]);

    if (spec->left)
        emit_repeat(out, ' ', pad);
}

/**
 * @brief Emit an integer conversion.
 *
 * @param magnitude  Absolute value to print.
 * @param negative   Whether to print a minus sign.
 * @param is_signed  Whether @c + / space flags apply.
 * @param base       8, 10, or 16.
 * @param upper      Uppercase hex digits.
 * @param prefix0x   Radix prefix to print before the digits ("" if none).
 */
static void emit_integer(struct fmt_out *out, const struct fmt_spec *spec,
                         uint64_t magnitude, bool negative, bool is_signed,
                         unsigned base, bool upper, const char *prefix0x)
{
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = upper ? digits_upper : digits_lower;

    // 22 octal digits cover a full 64-bit value
    char buf[24];
    int  len = 0;
    char *end = buf + sizeof(buf);

    // Precision 0 with value 0 prints no digits
    if (!(magnitude == 0 && spec->precision == 0))
        do
        {
            *--end = digits[magnitude % base];
            magnitude /= base;
            len++;
        } while (magnitude);

    int min_digits = spec->precision > 0 ? spec->precision : 0;

    // '#' with octal: guarantee a leading zero
    if (base == 8 && spec->alt && (len == 0 || *end != '0'))
        min_digits = MAX(min_digits, len + 1);

    char prefix[4];
    int  p = 0;
    if (negative)
        prefix[p++] = '-';
    else if (is_signed && spec->plus)
        prefix[p++] = '+';
    else if (is_signed && spec->space)
        prefix[p++] = ' ';
    for (const char *s = prefix0x; *s; ++s)
        prefix[p++] = *s;
    prefix[p] = '\0';

    emit_field(out, spec, true, prefix, end, len, min_digits);
}

// ── Formatter ──────────────────────────────────────────────────────────

size_t kvformat(format_sink_t sink, void *ctx,
                const char *fmt, __builtin_va_list args)
{
    struct fmt_out out = { .sink = sink, .ctx = ctx, .count = 0 };

    while (*fmt)
    {
        if (*fmt != '%')
        {
            emit(&out, *fmt++);
            continue;
        }

        const char *start = fmt++;  // for literal fallback
        struct fmt_spec spec = { .precision = -1, .length = LEN_INT };
    
        // Flags
        for (bool more = true; more; )
        {
            switch(*fmt)
            {
                case '-': spec.left  = true; fmt++; break;
                case '0': spec.zero  = true; fmt++; break;
                case '+': spec.plus  = true; fmt++; break;
                case ' ': spec.space = true; fmt++; break;
                case '#': spec.alt   = true; fmt++; break;
                default:  more = false;             break;
            }
        }

        // Width
        if (*fmt == '*')
        {
            int w = __builtin_va_arg(args, int);
            if (w < 0)
            {
                spec.left = true;
                w = -w;
            }
            spec.width = w;
            fmt++;
        }
        else
        {
            while (*fmt >= '0' && *fmt <= '9')
                spec.width = spec.width * 10 + (*fmt++ - '0');
        }

        // Precision
        if (*fmt == '.')
        {
            fmt++;
            if (*fmt == '*')
            {
                int prec = __builtin_va_arg(args, int);
                spec.precision = prec < 0 ? -1 : prec;
                fmt++;
            }
            else
            {
                spec.precision = 0;
                while (*fmt >= '0' && *fmt <= '9')
                    spec.precision = spec.precision * 10 + (*fmt++ - '0');
            }
        }

        // Length modifier
        switch (*fmt)
        {
            case 'h':
                fmt++;
                if (*fmt == 'h')
                {
                    fmt++;
                    spec.length = LEN_CHAR;
                }
                else
                    spec.length = LEN_SHORT;
                break;
            case 'l':
                fmt++;
                if (*fmt == 'l')
                    fmt++;
                spec.length = LEN_LONG;
                break;
            case 'z':
            case 'j':
            case 't':
                fmt++;
                spec.length = LEN_LONG;
                break;
            default:
            break;
        }

        // Conversion
        switch (*fmt)
        {
        case 'd':
        case 'i':
        {
            int64_t v;
            switch (spec.length)
            {
            case LEN_CHAR:  v = (signed char)__builtin_va_arg(args, int); break;
            case LEN_SHORT: v = (short)__builtin_va_arg(args, int);       break;
            case LEN_LONG:  v = __builtin_va_arg(args, long);             break;
            default:        v = __builtin_va_arg(args, int);              break;
            }
            uint64_t mag = v < 0 ? -(uint64_t)v : (uint64_t)v;
            emit_integer(&out, &spec, mag, v < 0, true, 10, false, "");
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o':
        {
            uint64_t v;
            switch(spec.length)
            {
                case LEN_CHAR:  v = (unsigned char)__builtin_va_arg(args, unsigned);
                                break;
                case LEN_SHORT: v = (unsigned short)__builtin_va_arg(args, unsigned);
                                break;
                case LEN_LONG:  v = __builtin_va_arg(args, unsigned long);
                                break;
                default:        v = __builtin_va_arg(args, unsigned);
                                break;
            }
            
            unsigned base = (*fmt == 'u') ? 10 : (*fmt == 'o') ? 8 : 16;
            const char *radix = "";
            if (spec.alt && base == 16 && v != 0)
                radix = (*fmt == 'X') ? "0X" : "0x";
        
            emit_integer(&out, &spec, v, false, false, base, *fmt == 'X', radix);
            break;
        }
        case 'p':
        {
            uintptr_t v = (uintptr_t)__builtin_va_arg(args, void *);
            emit_integer(&out, &spec, v, false, false, 16, false, "0x");
            break;
        }
        case 's':
        {
            const char *s = __builtin_va_arg(args, const char *);
            if (!s)
                s = "(null)";
            size_t len = spec.precision >= 0
                       ? strnlen(s, (size_t)spec.precision)
                       : strlen(s);
            emit_field(&out, &spec, false, "", s, (int)len, 0);
            break;
        }
        case 'c':
        {
            char c = (char)__builtin_va_arg(args, int);
            emit_field(&out, &spec, false, "", &c, 1, 0);
            break;
        }
        case '%':
            emit(&out, '%');
            break;
        default:
            // Print literally
            for (const char *s = start; s < fmt; ++s)
                emit(&out, *s);
            if (*fmt)
                emit(&out, *fmt);
            break;
        }

        if (*fmt)
            fmt++;
    }

    return out.count;
}

// ── Buffer formatting ──────────────────────────────────────────────────

/// Sink state for @c kvsnprintf.
struct snprintf_ctx
{
    char  *buf;
    size_t size;
    size_t pos;   ///< Characters produced (may exceed @c size);
};

static void snprintf_sink(void *ctx, char c)
{
    struct snprintf_ctx *s = (struct snprintf_ctx *)ctx;
    if (s->pos + 1 < s->size)
        s->buf[s->pos] = c;
    s->pos++;
}

size_t kvsnprintf(char *buf, size_t size, const char *fmt,
                  __builtin_va_list args)
{
    struct snprintf_ctx ctx = { .buf = buf, .size = size, .pos = 0 };
    size_t n = kvformat(snprintf_sink, &ctx, fmt, args);

    if (size)
        buf[n < size ? n : size - 1] = '\0';
    return n;
}

size_t ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    size_t n = kvsnprintf(buf, size, fmt, args);
    __builtin_va_end(args);
    return n;
}

