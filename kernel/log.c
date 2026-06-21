/**
 * @file log.c
 * @brief Runtime log level management.
 */

#include "log.h"

/// Default level: show everything except DEBUG.
int current_log_level = LOG_INFO;

void log_set_level(int level)
{
    current_log_level = level;
}

int log_get_level(void)
{
    return current_log_level;
}

