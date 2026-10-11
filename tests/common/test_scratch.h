/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-11 */
/* Per-process scratch directory of the C test suites, under build/. */
#ifndef TEST_SCRATCH_H
#define TEST_SCRATCH_H

#include <limits.h>
#include <stddef.h>

/**
 * @file test_scratch.h
 * @brief Every path a C test uses lives under build/tests/<category>/work-<pid>.
 *
 * A test never uses /tmp: a fixed path is read back from the previous run and
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

#endif /* TEST_SCRATCH_H */
