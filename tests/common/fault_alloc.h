/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-19 */
/* Failing allocator hook of the unit tests (EMAIL-139). */
#ifndef FAULT_ALLOC_H
#define FAULT_ALLOC_H

/**
 * @file fault_alloc.h
 * @brief Makes the Nth allocation of product code fail, to test out-of-memory
 *        branches.
 *
 * The unit test runner is linked with `-Wl,--wrap=malloc,--wrap=calloc,
 * --wrap=realloc`, so every malloc, calloc and realloc made by code linked
 * into it (the product sources and the tests) goes through fault_alloc.c.
 * Allocations made inside shared libraries (OpenSSL, libc) are not affected.
 *
 * Usage, the "fail every allocation point in turn" sweep:
 * @code
 * for (long k = 1; k < 500; k++) {
 *     fault_alloc_arm(k);
 *     int rc = function_under_test();
 *     int fired = fault_alloc_disarm();
 *     ... check rc is a clean failure when fired ...
 *     if (!fired) break;      // k is past the last allocation: all swept
 * }
 * @endcode
 * Leaks on the failure paths are reported by the leak checker at exit.
 */

/** Fail the @p nth allocation (1 = the next one) from now on, once. */
void fault_alloc_arm(long nth);

/** Stop injecting; returns 1 when the armed allocation failure happened. */
int fault_alloc_disarm(void);

#endif /* FAULT_ALLOC_H */
