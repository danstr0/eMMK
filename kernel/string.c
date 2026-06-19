/**
 * @file string.c
 * @brief Freestanding string and memory manipulation.
 */

#include "string.h"

// ── Memory operations ──────────────────────────────────────────────────

void *memset(void *dest, int c, size_t n)
{
    uint8_t *p = (uint8_t *)dest;
    while (n--)
        *p++ = (uint8_t)c;
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
        *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (d < s)
        while (n--)
            *d++ = *s++;
    else
    {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;

    while (n--)
    {
        if (*p1 != *p2)
            return *p1 - *p2;
        p1++;
        p2++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    while (n--)
    {
        if (*p == (uint8_t)c)
            return (void *)p;
        p++;
    }
    return NULL;
}

// ── String examination ─────────────────────────────────────────────────

size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len])
        len++;
    return len;
}

size_t strnlen(const char *s, size_t maxlen)
{
    size_t len;
    for (len = 0; len < maxlen; len++)
        if (!s[len])
            break;
    return len;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2))
    {
        s1++;
        s2++;
    }
    return *(const uint8_t *)s1 - *(const uint8_t *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    if (n == 0)
        return 0;

    while(n && *s1 && (*s1 == *s2))
    {
        s1++;
        s2++;
        n--;
    }

    if (n == 0)
        return 0;

    return *(const uint8_t *)s1 - *(const uint8_t *)s2;
}

char *strchr(const char *s, int c)
{
    for (;; s++)
    {
        if (*s == (char)c)
            return (char *)s;
        if (*s == '\0')
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    do
    {
        if (*s == (char)c)
            last = s;
    } while (*s++);
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t nlen = strlen(needle);

    if (nlen == 0)
        return (char *)haystack;

    for (; *haystack; ++haystack)
        if (*haystack == *needle && strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
    return NULL;
}

// ── Safe string copy / concatenation ───────────────────────────────────

size_t strlcpy(char *dest, const char *src, size_t size)
{
    size_t len = strlen(src);

    if (size)
    {
        size_t copy_len = (len >= size) ? size - 1 : len;
        memcpy(dest, src, copy_len);
        dest[copy_len] = '\0';
    }
    return len;
}

size_t strlcat(char *dest, const char *src, size_t size)
{
    size_t dlen = strnlen(dest, size);
    size_t slen = strlen(src);

    if (dlen == size)
        return size + slen;

    if (slen < size - dlen)
        memcpy(dest + dlen, src, slen + 1);
    else
    {
        memcpy(dest + dlen, src, size - dlen - 1);
        dest[size - 1] = '\0';
    }
    return dlen + slen;
}

