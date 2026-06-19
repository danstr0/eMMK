/**
 * @file test.h
 * @brief Minimal kernel test framework.
 *
 * Provides assertion macros that report failures over the serial console
 * and a QEMU exit mechanism so the host can detect pass/fail.
 *
 * @par Usage
 * Each test suite is a function returning @c uint32_t (the failure count):
 * @code
 *   uint32_t test_example(void)
 *   {
 *       TEST_BEGIN("example");
 *
 *       TEST_ASSERT(1 + 1 == 2,      "basic arithmetic");
 *       TEST_ASSERT_EQ(4, 2 + 2,     "addition");
 *       TEST_ASSERT_MEMEQ(a, b, 16,  "buffers match");
 *
 *       TEST_END();   // prints summary and returns failure count
 *   }
 * @endcode
 *
 * @par QEMU integration
 * Build with @c -DKERNEL_TEST and run QEMU with:
 * @code
 *   -device isa-debug-exit,iobase=0xf4,iosize=0x04
 * @endcode
 * The test runner calls @c qemu_exit_success() or @c qemu_exit_failure(),
 * which write to that port. QEMU's exit code is @c (value<<1)|1, so
 * success = exit 1, failure = exit 3. The Makefile adjusts accordingly.
 */

#ifndef EMMK_TEST_H
#define EMMK_TEST_H

#include "types.h"
#include "serial.h"
#include "string.h"
#include "io.h"

/// @name QEMU exit
/// @{

/// I/O port for @c isa-debug-exit device (must match QEMU command line).
#define QEMU_EXIT_PORT  0xF4

/**
 * @brief Terminate QEMU with exit @c (code<<1)|1.
 *
 * Writing @c 0x00 produces host exit code 1 (success).
 * Writing @c 0x01 produces host exit code 3 (failure).
 */
NORETURN static inline void qemu_exit(uint32_t code)
{
    outl(QEMU_EXIT_PORT, code);
    for (;;)
        __asm__ volatile("hlt");
}

static inline NORETURN void qemu_exit_success(void) { qemu_exit(0x00); }
static inline NORETURN void qemu_exit_failure(void) { qemu_exit(0x01); }
/// @}

/// @name Test suite macros
/// @{

/**
 * @brief Open a test suite.
 *
 * Declares a local counter and prints the suite name. Must appear at the
 * top of a suite function, before any local variables.
 */
#define TEST_BEGIN(name)                           \
    uint32_t __test_passed = 0, __test_failed = 0; \
    const char *__test_suite UNUSED = (name);      \
    kprintf("[suite] %s\n", (name))

/**
 * @brief Assert that @p cond is true.
 *
 * On failure, prints the file, line, and @p msg to the serial console.
 */
#define TEST_ASSERT(cond, msg)                             \
do {                                                       \
    if (cond)                                              \
        __test_passed++;                                   \
    else                                                   \
    {                                                      \
        __test_failed++;                                   \
        kprintf("  FAIL  %s:%d: %s\n", __FILE__, __LINE__, \
                (msg));                                    \
    }                                                      \
} while (0)

/// Assert that @p a == @p b.
#define TEST_ASSERT_EQ(a, b, msg)   TEST_ASSERT((a) == (b), msg)

/// Assert that @p a != @p b.
#define TEST_ASSERT_NEQ(a, b, msg)  TEST_ASSERT((a) != (b), msg)

/// Assert that @p n bytes at @p a and @p b are identical.
#define TEST_ASSERT_MEMEQ(a, b, n, msg) \
    TEST_ASSERT(memcmp((a), (b), (n)) == 0, msg)

/// Assert that @p a and @p are equal C strings.
#define TEST_ASSERT_STREQ(a, b, msg) \
    TEST_ASSERT(strcmp((a), (b)) == 0, msg)

/**
 * @brief Close a test suite.
 *
 * Prints the pass/fail summary and returns the failure count to the caller.
 */
#define TEST_END()                                \
do {                                              \
    kprintf("  result: %u passed, %u failed\n\n", \
            __test_passed, __test_failed);        \
    return __test_failed;                         \
} while (0)

/// @}

#endif // EMMK_TEST_H

