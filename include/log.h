/**
 * @file log.h
 * @brief Leveled logging with per-core identification.
 *
 * Provides @c klog(), a drop-in companion to @c kprintf() that adds:
 *   - A severity level (@c ERROR / @c WARN / @c INFO / @c DEBUG) checked
 *     against a runtime threshold — messages below the threshold are
 *     compiled to a cheap branch-not-taken.
 *   - A per-core prefix @c [core_id:tag] so output from concurrent
 *     cores is unambiguous.
 *
 * @par Usage
 * @code
 *   klog(LOG_INFO, "ETH", "frame received, len=%u\n", len);
 *   // → [2:ETH] frame received, len=64
 *
 *   // Before entering the hot path:
 *   log_set_level(LOG_WARN);   // silences INFO / DEBUG
 * @endcode
 *
 * The @c tag parameter is a short, developer-chosen label — typically
 * the subsystem name ("ARP", "ICMP", "UDP") or the core's role name.
 * The core index is prepended automatically.
 */

#ifndef EMMK_LOG_H
#define EMMK_LOG_H

#include "serial.h"
#include "smp.h"

/// @name Log levels (lower = more critical)
/// @{
#define LOG_NONE   0  ///< Suppress all output.
#define LOG_ERROR  1  ///< Errors that prevent correct operation.
#define LOG_WARN   2  ///< Unusual conditions worth investigating.
#define LOG_INFO   3  ///< Normal operational messages (default).
#define LOG_DEBUG  4  ///< Verbose output for development.
/// @}

/// @name Level control
/// @{

/// Current threshold — messages with level > this are suppressed.
extern int current_log_level;

/// Set the runtime log level (e.g., @c LOG_WARN to silence INFO/DEBUG).
void log_set_level(int level);

/// @return The current log level.
int log_get_level(void);
/// @}

/// @name Logging macros
/// @{

/**
 * @brief Core logging macro.
 *
 * Emits @c [core_id:tag] prefix followed by the formatted message,
 * but only if @p level ≤ @c current_log_level. The branch is
 * predictable (almost always not-taken on the hot path once the
 * level is lowered), so the cost of a silenced @c klog is a single
 * comparison.
 *
 * @param level  One of @c LOG_ERROR .. @c LOG_DEBUG.
 * @param tag    Short string identifying the subsystem or role.
 * @param fmt    printf-style format string.
 */
#define klog(level, tag, fmt, ...)                    \
do {                                                  \
    if ((level) <= current_log_level)                 \
        kprintf("[%d:%s] " fmt, smp_core_id(), (tag), \
                ##__VA_ARGS__);                       \
} while (0)

// Convenience wrappers
#define klog_error(tag, fmt, ...) klog(LOG_ERROR, tag, fmt, ##__VA_ARGS__)
#define klog_warn(tag, fmt, ...)  klog(LOG_WARN,  tag, fmt, ##__VA_ARGS__)
#define klog_info(tag, fmt, ...)  klog(LOG_INFO,  tag, fmt, ##__VA_ARGS__)
#define klog_debug(tag, fmt, ...) klog(LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
/// @}

#endif // EMMK_LOG_H

