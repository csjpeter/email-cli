/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-24 */
/* Interface of the logger. */
#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>

/**
 * @file logger.h
 * @brief Detailed logging system with rotation and level support.
 */

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

/**
 * @brief Initializes the logging system.
 * @param log_file_path Path to the log file (e.g., ~/.cache/email-cli/logs/session.log).
 * @param level Minimum log level to display/record.
 * @return 0 on success, -1 on failure.
 */
int logger_init(const char *log_file_path, LogLevel level);

/**
 * @brief Logs a message with a specific level.
 * @param level The severity level of the log.
 * @param format Format string (printf style).
 */
void logger_log(LogLevel level, const char *format, ...);

/**
 * @brief Closes the logging system.
 */
void logger_close(void);

/**
 * @brief Cleans up all log files in the log directory.
 * @param log_dir Path to the log directory.
 * @return 0 on success, -1 on failure.
 */
int logger_clean_logs(const char *log_dir);

/**
 * @brief Enables or disables the mirroring of ERROR logs to stderr.
 * @param enable 1 to enable, 0 to disable.
 */
void logger_set_stderr(int enable);

/**
 * @brief Number of ERROR messages written to stderr so far (EMAIL-105).
 *
 * A caller that wants to know whether a failing call already told the user
 * why compares the value before and after the call.
 */
unsigned logger_stderr_error_count(void);

/**
 * @brief Count one error the caller wrote to stderr itself (EMAIL-105), for
 *        a message that must not depend on the logger's stderr mirror.
 */
void logger_note_stderr_error(void);

#endif /* LOGGER_H */
