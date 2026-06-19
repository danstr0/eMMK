/**
 * @file test_string.c
 * @brief Tests for string.h: memory operations, string examination,
 *        and safe copy/concatenation.
 */

#include "test.h"

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

    TEST_END();
}

