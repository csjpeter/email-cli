/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-23 */
/* The factor by which the PTY tests stretch their waits (EMAIL-224). */
/**
 * @file wait_scale.h
 * @brief Waits of the PTY tests, scaled for programs run under Valgrind.
 *
 * `manage.sh valgrind-all` runs every program under memcheck, which is ten to
 * fifty times slower; it sets EMAIL_TEST_WAIT_SCALE so that a wait for a screen
 * (a sync, a TLS handshake) is as many times longer.  Unset, the factor is 1
 * and the tests wait as they always did.
 */
#ifndef PTY_WAIT_SCALE_H
#define PTY_WAIT_SCALE_H

#include <stdlib.h>

/** @brief The factor of EMAIL_TEST_WAIT_SCALE, 1 when unset or not a positive number. */
static inline int wait_scale(void) {
    const char *e = getenv("EMAIL_TEST_WAIT_SCALE");
    int n = e ? atoi(e) : 1;
    return n > 0 ? n : 1;
}

#endif
