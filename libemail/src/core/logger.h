/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-24 */
/* Interface of the traffic log and of the error output. */
#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stddef.h>

/**
 * @file logger.h
 * @brief The traffic log (`--debug` only) and the stderr error output.
 *
 * There is no session log (EMAIL-284).  What a failure has to say is in the
 * error message on stderr.  Only when `--debug` is on the command line a file
 * is made that holds the protocol traffic (IMAP, SMTP, Gmail REST), with every
 * credential masked, for debugging the protocols.
 */

/**
 * @brief Opens the traffic log (appends; rotates at 5 MB, keeps five old files).
 * @param log_file_path Path of the file (e.g. ~/.cache/email-cli/logs/session.log).
 * @return 0 on success, -1 on failure.
 */
int logger_init(const char *log_file_path);

/**
 * @brief Without a traffic log, still show errors on stderr (EMAIL-240).
 *
 * The programs call it when `--debug` is not given; a program that never
 * calls it and opens no file stays silent, as the library tests expect.
 */
void logger_stderr_only(void);

/**
 * @brief Whether `--debug` is among the arguments.
 *
 * `--debug` is removed from argv, so no command has to know it.  The one way
 * to ask for the traffic log is this option (no environment variable).
 */
int logger_debug_requested(int *argc, char **argv);

/**
 * @brief Writes one traffic line (printf style) to the traffic log.
 *
 * Does nothing when the log is not open.  The caller masks credentials
 * before the call; see logger_mask_secret().
 */
void logger_traffic(const char *format, ...);

/**
 * @brief Whether the traffic log is open (so a caller can skip building a line).
 */
int logger_traffic_enabled(void);

/**
 * @brief Masks the credential of one protocol line in place (EMAIL-284).
 *
 * Recognises, case-insensitively: `LOGIN user pass`, `AUTHENTICATE <mech> <data>`
 * and `AUTH <mech> <data>` (XOAUTH2 / PLAIN / LOGIN bearer or base64 data),
 * a bare base64 continuation after an AUTH exchange is not recognisable and
 * is masked by the caller that knows it is one, and `Authorization: <scheme> <value>`
 * headers, plus `access_token` / `refresh_token` / `client_secret` JSON or form
 * values.  The secret is replaced by `xxxxx`.
 *
 * @param line Modified in place.
 * @param cap  Size of the buffer behind @p line.
 */
void logger_mask_secret(char *line, size_t cap);

/**
 * @brief Writes an error line to stderr as `ERROR: ...` and counts it.
 *
 * Shown only when the traffic log is open or logger_stderr_only() was called,
 * and logger_set_stderr(1) is in effect.
 */
void logger_error(const char *format, ...);

/**
 * @brief Closes the traffic log.
 */
void logger_close(void);

/**
 * @brief Cleans up all log files in the log directory.
 * @param log_dir Path to the log directory.
 * @return 0 on success, -1 on failure.
 */
int logger_clean_logs(const char *log_dir);

/**
 * @brief Enables or disables the stderr error output.
 * @param enable 1 to enable, 0 to disable.
 */
void logger_set_stderr(int enable);

/**
 * @brief Number of errors written to stderr so far (EMAIL-105).
 *
 * A caller that wants to know whether a failing call already told the user
 * why compares the value before and after the call.
 */
unsigned logger_stderr_error_count(void);

/**
 * @brief Count one error the caller wrote to stderr itself (EMAIL-105), for
 *        a message that must not depend on the logger's stderr output.
 */
void logger_note_stderr_error(void);

#endif /* LOGGER_H */
