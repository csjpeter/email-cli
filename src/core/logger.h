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

#endif // LOGGER_H
