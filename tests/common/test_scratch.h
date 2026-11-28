/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-11 */
/* Per-process scratch directory of the C test suites, under build/. */
#ifndef TEST_SCRATCH_H
#define TEST_SCRATCH_H

#include <limits.h>
#include <stddef.h>
#include <sys/types.h>

/**
 * @file test_scratch.h
 * @brief Every path a C test uses lives under build/tests/<category>/work-<pid>.
 *
 * A test never uses the shared system temp directory: a fixed path is read back from the previous run and
 * collides between users.  The directory is removed when the process ends
 * (atexit, so the failure path too); a killed run leaves it under build/,
 * where `./manage.sh clean` takes it.
 *
 * Implemented in test_scratch.c, which every test target compiles with
 * TEST_BUILD_DIR and TEST_CATEGORY defined by CMake.
 */

/** @brief The process's scratch directory; created on first use. */
const char *test_scratch_dir(void);

/**
 * @brief Absolute path of @p rel inside the scratch directory ("" is the
 *        directory itself).
 *
 * The same @p rel always yields the same pointer, which stays valid for the
 * life of the process, so it can be kept in a `const char *`.
 */
const char *test_path(const char *rel);

/** @brief Shorthand for test_path(); adjacent string literals concatenate. */
#define TP(...) test_path(__VA_ARGS__)

/**
 * @brief Remove a tree below the scratch directory.
 *
 * Refuses (returns -1) any path that is not inside this process's scratch
 * directory.  A missing path is success.
 */
int test_rmtree(const char *path);

/**
 * @brief mkdtemp() inside the scratch directory.
 * @return buf, or NULL (and a message on stderr) on failure.
 */
char *test_mkdtemp(char *buf, size_t n, const char *prefix);

/** @brief Set HOME and the three XDG_* variables from one fixture home. */
void test_set_home(const char *home);

/**
 * @brief Start a mock server and wait until it has bound its port.
 *
 * The mock is started with `<prefix>_PORT` set to @p want_port (or unset when
 * 0, so the kernel picks one) and `<prefix>_PORT_FILE` naming a file in the
 * scratch directory; the call returns when the mock has written the port it
 * really got there.  The mock's stdout and stderr go to a log in the scratch
 * directory.  The environment the caller set (MOCK_*_SUBJECT and the like) is
 * inherited, and the working directory is kept.
 *
 * @param bin       Path of the mock binary.
 * @param prefix    Variable prefix, e.g. "MOCK_IMAP".
 * @param want_port 0, or the port to bind (a restart on the same port).
 * @param port_out  Receives the bound port.
 * @return the pid, or -1 with a message (and the mock's log) on stderr.
 */
pid_t test_spawn_mock(const char *bin, const char *prefix, int want_port,
                      int *port_out);

/** @brief The log file of a mock started by test_spawn_mock(), by its pid (NULL if unknown). */
const char *test_mock_log(pid_t pid);

/** @brief How many lines of the file @p path contain @p needle (0 if unreadable). */
int test_log_count(const char *path, const char *needle);

/**
 * @brief Waits up to @p ms for @p want lines of @p path to contain @p needle.
 * @return 0 when they do, -1 on timeout.
 */
int test_log_wait(const char *path, const char *needle, int want, int ms);

/**
 * @brief The poll interval of every wait of the C tests, in milliseconds.
 *
 * A wait checks its condition this often and ends the moment it holds; its
 * limit is only a generous ceiling (policies/testing-model.md, "Wait for a
 * condition, not for a clock"; EMAIL-324).
 */
#define TEST_POLL_MS 10

/**
 * @brief Waits until @p cond(@p arg) returns non-zero, at most @p ms.
 *
 * The one poll loop of the C tests: the condition is checked at once and then
 * every TEST_POLL_MS milliseconds.
 * @return 0 when the condition holds, -1 when @p ms passed without it.
 */
int test_wait_until(int (*cond)(void *arg), void *arg, int ms);

/**
 * @brief Waits for the child @p pid to exit, at most @p ms, and reaps it.
 *
 * A child still alive at the limit is killed (SIGKILL) and reaped, so a test
 * that failed before it talked to its mock does not hang the suite.
 * @param status receives the wait status (may be NULL).
 * @return 0 when the child exited by itself, 1 when it had to be killed,
 *         -1 when @p pid is not a child of ours (or <= 0).
 */
int test_wait_child(pid_t pid, int *status, int ms);

/**
 * @brief Waits until something accepts a TCP connection on 127.0.0.1:@p port.
 * @return 0 when a connect succeeds, -1 when @p ms passed without it.
 */
int test_wait_port(int port, int ms);

/**
 * @brief Waits until the file @p path exists, at most @p ms.
 * @return 0 when it exists, -1 on timeout.
 */
int test_wait_file(const char *path, int ms);

/**
 * @brief Waits until the main thread of @p pid sleeps (state S in /proc).
 *
 * A child that must act only once its parent waits for input (types keys,
 * sends SIGWINCH) uses it instead of guessing how long the parent takes to
 * get there: a process blocked in read() or poll() sleeps.
 * @return 0 when it sleeps, -1 on timeout or when /proc cannot be read.
 */
int test_wait_sleeping(pid_t pid, int ms);

/** @brief A loopback port nobody listens on at this moment. */
int test_unused_port(void);

/**
 * @brief Whether the test named @p name is selected by $TEST_FILTER.
 *
 * Unset or empty selects everything; otherwise the name must contain the
 * variable's value.  When a filter is set and nothing was selected by the end
 * of the process, the process exits non-zero: a filter that matches no test
 * must not look like a pass.
 */
int test_selected(const char *name);

#endif /* TEST_SCRATCH_H */
