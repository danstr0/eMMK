/**
 * @file test_string.c
 * @brief Tests for string.h: memory operations, string examination,
 *        and safe copy/concatenation.
 */

#include "test.h"
#include "format.h"

uint32_t test_string(void)
{
    TEST_BEGIN("string");

    // ── memset ──────────────────────────────────────────
    {
        uint8_t buf[32];
        memset(buf, 0xAB, sizeof(buf));

        bool ok = true;
        for (size_t i = 0; i < sizeof(buf); ++i)
            if (buf[i] != 0xAB) { ok = false; break; }
        TEST_ASSERT(ok, "memset fill 0xAB");

        memset(buf, 0, sizeof(buf));
        ok = true;
        for (size_t i = 0; i < sizeof(buf); ++i)
            if (buf[i] != 0) { ok = false; break; }
        TEST_ASSERT(ok, "memset zero");
    }

    // ── memcpy ──────────────────────────────────────────
    {
        const char src[] = "hello, kernel";
        char dst[32];
        memset(dst, 0, sizeof(dst));
        memcpy(dst, src, sizeof(src));
        TEST_ASSERT_MEMEQ(dst, src, sizeof(src), "memcpy basic");

        // zero-length copy should not modify dest
        dst[0] = 'X';
        memcpy(dst, src, 0);
        TEST_ASSERT_EQ(dst[0], 'X', "memcpy zero length");
    }

    // ── memmove (overlapping) ───────────────────────────
    {
        // forward overlap: src < dest
        char buf[16] = "abcdefghij";
        memmove(buf + 2, buf, 8);
        TEST_ASSERT_MEMEQ(buf + 2, "abcdefgh", 8,
                          "memmove forward overlap");

        // backward overlap: dest < src
        char buf2[16] = "abcdefghij";
        memmove(buf2, buf2 + 2, 8);
        TEST_ASSERT_MEMEQ(buf2, "cdefghij", 8,
                          "memmove backward overlap");
    }

    // ── memcmp ──────────────────────────────────────────
    {
        TEST_ASSERT_EQ(memcmp("abc", "abc", 3), 0,
                       "memcmp equal");
        TEST_ASSERT(memcmp("abc", "abd", 3) < 0,
                    "memcmp less");
        TEST_ASSERT(memcmp("abd", "abc", 3) > 0,
                    "memcmp greater");
        TEST_ASSERT_EQ(memcmp("abc", "xyz", 0), 0,
                       "memcmp zero length");
    }

    // ── memchr ──────────────────────────────────────────
    {
        const char data[] = "abcdef";
        TEST_ASSERT_EQ((uintptr_t)memchr(data, 'c', 6),
                       (uintptr_t)&data[2], "memchr found");
        TEST_ASSERT_EQ((uintptr_t)memchr(data, 'z', 6),
                       (uintptr_t)NULL, "memchr not found");
        TEST_ASSERT_EQ((uintptr_t)memchr(data, 'a', 6),
                       (uintptr_t)&data[0], "memchr first byte");
    }

    // ── strlen / strnlen ────────────────────────────────
    {
        TEST_ASSERT_EQ(strlen(""),      0, "strlen empty");
        TEST_ASSERT_EQ(strlen("hello"), 5, "strlen hello");

        TEST_ASSERT_EQ(strnlen("hello", 10), 5,
                       "strnlen within limit");
        TEST_ASSERT_EQ(strnlen("hello", 3),  3,
                       "strnlen capped");
        TEST_ASSERT_EQ(strnlen("hello", 0),  0,
                       "strnlen zero limit");
    }

    // ── strcmp / strncmp ────────────────────────────────
    {
        TEST_ASSERT_EQ(strcmp("abc", "abc"),  0, "strcmp equal");
        TEST_ASSERT(strcmp("abc", "abd") < 0,    "strcmp less");
        TEST_ASSERT(strcmp("abd", "abc") > 0,    "strcmp greater");
        TEST_ASSERT(strcmp("abc", "abcd") < 0,   "strcmp prefix shorter");

        TEST_ASSERT_EQ(strncmp("abcX", "abcY", 3), 0,
                       "strncmp bounded equal");
        TEST_ASSERT_EQ(strncmp("abc", "xyz", 0), 0,
                       "strncmp zero n");
    }

    // ── strchr / strrchr ────────────────────────────────
    {
        const char *s = "abcabc";
        TEST_ASSERT_EQ((uintptr_t)strchr(s, 'b'),
                       (uintptr_t)&s[1], "strchr first");
        TEST_ASSERT_EQ((uintptr_t)strchr(s, 'z'),
                       (uintptr_t)NULL, "strchr not found");
        // strchr with '\0' should find the terminator
        TEST_ASSERT_EQ((uintptr_t)strchr(s, '\0'),
                       (uintptr_t)&s[6], "strchr NUL");

        TEST_ASSERT_EQ((uintptr_t)strrchr(s, 'b'),
                       (uintptr_t)&s[4], "strrchr last");
        TEST_ASSERT_EQ((uintptr_t)strrchr(s, 'z'),
                       (uintptr_t)NULL, "strrchr not found");
    }

    // ── strstr ──────────────────────────────────────────
    {
        const char *hay = "hello world";
        TEST_ASSERT_EQ((uintptr_t)strstr(hay, "world"),
                       (uintptr_t)&hay[6], "strstr found");
        TEST_ASSERT_EQ((uintptr_t)strstr(hay, "xyz"),
                       (uintptr_t)NULL, "strstr not found");
        TEST_ASSERT_EQ((uintptr_t)strstr(hay, ""),
                       (uintptr_t)hay, "strstr empty needle");
    }

    // ── strlcpy ─────────────────────────────────────────
    {
        char dst[8];

        // Normal copy (fits)
        size_t ret = strlcpy(dst, "abc", sizeof(dst));
        TEST_ASSERT_STREQ(dst, "abc", "strlcpy normal copy");
        TEST_ASSERT_EQ(ret, 3, "strlcpy returns src length");

        // Truncation
        ret = strlcpy(dst, "longstring", sizeof(dst));
        TEST_ASSERT_EQ(dst[7], '\0', "strlcpy NUL terminates");
        TEST_ASSERT_MEMEQ(dst, "longstr", 7, "strlcpy truncated content");
        TEST_ASSERT_EQ(ret, 10, "strlcpy returns full src length");

        // Zero-size destination
        dst[0] = 'X';
        ret = strlcpy(dst, "abc", 0);
        TEST_ASSERT_EQ(dst[0], 'X', "strlcpy size 0 no write");
        TEST_ASSERT_EQ(ret, 3, "strlcpy size 0 return");
    }

    // ── strlcat ─────────────────────────────────────────
    {
        char dst[12];

        // Normal append
        strlcpy(dst, "hello", sizeof(dst));
        size_t ret = strlcat(dst, " world", sizeof(dst));
        TEST_ASSERT_STREQ(dst, "hello world", "strlcat normal");
        TEST_ASSERT_EQ(ret, 11, "strlcat returns total length");

        // Truncating append
        strlcpy(dst, "hello", sizeof(dst));
        ret = strlcat(dst, " wonderful world", sizeof(dst));
        TEST_ASSERT_EQ(dst[11], '\0', "strlcat truncated NUL");
        TEST_ASSERT_EQ(ret, 21, "strlcat truncated return");

        // Already-full buffer
        char full[4] = "abc";
        ret = strlcat(full, "def", sizeof(full));
        TEST_ASSERT_STREQ(full, "abc", "strlcat full buf unchanged");
        TEST_ASSERT_EQ(ret, 6, "strlcat full buf return");
    }

    // ── memzero ─────────────────────────────────────────
    {
        uint8_t buf[16];
        memset(buf, 0xFF, sizeof(buf));
        memzero(buf, sizeof(buf));

        bool ok = true;
        for (size_t i = 0; i < sizeof(buf); ++i)
            if (buf[i] != 0) { ok = false; break; }
        TEST_ASSERT(ok, "memzero clears buffer");
    }

    // ── Out-of-line mem* (runtime sizes) ────────────────
    {
        static uint8_t a[4096 + 64], b[4096 + 64];
        volatile size_t n = 4096 + 3;

        for (size_t i = 0; i < sizeof(a); ++i)
            a[i] = (uint8_t)(i * 7);

        memset(b, 0, sizeof(b));
        memcpy(b + 1, a + 2, n);
        bool ok = (b[0] == 0) && (b[n + 1] == 0);
        for (size_t i = 0; ok && i < n; ++i)
            ok = (b[i + 1] == a[i + 2]);
        TEST_ASSERT(ok, "memcpy runtime size, unaligned, bounded");

        memset(b + 5, 0x5A, n);
        ok = (b[4] != 0x5A || a[2] == 0x5A) && (b[n + 5] == 0);
        for (size_t i = 0; ok && i < n; ++i)
            ok = (b[i + 5] == 0x5A);
        TEST_ASSERT(ok, "memset runtime size");

        // Overlap, dest above src (backward path)
        volatile size_t m = 100;
        for (size_t i = 0; i < 200; ++i)
            b[i] = (uint8_t)i;
        memmove(b + 10, b, m);
        ok = true;
        for (size_t i = 0; ok && i < m; ++i)
            ok = (b[i + 10] == (uint8_t)i);
        TEST_ASSERT(ok, "memmove overlap, dest > src");

        // Overlap, dest below src (forward path)
        for (size_t i = 0; i < 200; ++i)
            b[i] = (uint8_t)i;
        memmove(b, b + 10, m);
        ok = true;
        for (size_t i = 0; ok && i < m; ++i)
            ok = (b[i] == (uint8_t)(i + 10));
        TEST_ASSERT(ok, "memmove overlap, dest < src");
    }

    // ── ksnprintf: flags, width, precision, length ──────
    {
        char buf[64];

        ksnprintf(buf, sizeof(buf), "%08x", 0xbeefu);
        TEST_ASSERT_STREQ(buf, "0000beef", "fmt %08x zero pad");

        ksnprintf(buf, sizeof(buf), "%02x:%02x", 1u, 0xabu);
        TEST_ASSERT_STREQ(buf, "01:ab", "fmt %02x MAC-style");

        ksnprintf(buf, sizeof(buf), "%-5d|", 42);
        TEST_ASSERT_STREQ(buf, "42   |", "fmt %-5d left justify");
    
        ksnprintf(buf, sizeof(buf), "%5s", "ab");
        TEST_ASSERT_STREQ(buf, "   ab", "fmt %5s right justify");
    
        ksnprintf(buf, sizeof(buf), "%.3s", "abcdef");
        TEST_ASSERT_STREQ(buf, "abc", "fmt %.3s precision");
    
        ksnprintf(buf, sizeof(buf), "%*d|%-*d", 4, 7, 3, 1);
        TEST_ASSERT_STREQ(buf, "   7|1  ", "fmt * width");
    
        ksnprintf(buf, sizeof(buf), "%+d % d %d", 5, 5, -5);
        TEST_ASSERT_STREQ(buf, "+5  5 -5", "fmt sign flags");

        ksnprintf(buf, sizeof(buf), "%ld", (long)(-9223372036854775807L - 1));
        TEST_ASSERT_STREQ(buf, "-9223372036854775808", "fmt INT64_MIN");

        ksnprintf(buf, sizeof(buf), "%lu", 18446744073709551615UL);
        TEST_ASSERT_STREQ(buf, "18446744073709551615", "fmt UINT64_MAX");

        ksnprintf(buf, sizeof(buf), "%#x %#x %#o", 255u, 0u, 8u);
        TEST_ASSERT_STREQ(buf, "0xff 0 010", "fmt # alternate forms");

        ksnprintf(buf, sizeof(buf), "%.5d|%.0d|", 42, 0);
        TEST_ASSERT_STREQ(buf, "00042||", "fmt integer precision");

        ksnprintf(buf, sizeof(buf), "%zu %hhu %hd", (size_t)123, 300, 70000);
        TEST_ASSERT_STREQ(buf, "123 44 4464", "fmt z / hh / h lengths");

        ksnprintf(buf, sizeof(buf), "%p %c %%", (void *)0x1000, 'k');
        TEST_ASSERT_STREQ(buf, "0x1000 k %", "fmt %p %c %%");

        const char *volatile null_str = NULL;
        ksnprintf(buf, sizeof(buf), "%s", null_str);
        TEST_ASSERT_STREQ(buf, "(null)", "fmt NULL string");

        // Unknown conversion is printed literally and consumes nothing.
        // (Non-literal format so -Wformat does not reject it.)
        const char *volatile unknown = "%q|%d";
        ksnprintf(buf, sizeof(buf), unknown, 5);
        TEST_ASSERT_STREQ(buf, "%q|5", "fmt unknown conversion");

        size_t ret = ksnprintf(buf, 4, "%s", "abcdef");
        TEST_ASSERT_STREQ(buf, "abc", "ksnprintf truncates");
        TEST_ASSERT_EQ(ret, 6, "ksnprintf returns untruncated length");
    }

    TEST_END();
}

