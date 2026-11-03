/*
 * PS5 Sync Clock - logging and console notifications.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Write a line to the payload stdout (visible in the payload loader
 * console output).
 */
void log_message(const char* format, ...);

/*
 * Show a PS5 system notification popup. `fmt` uses printf conversions.
 */
void notify_message(const char* format, ...);
