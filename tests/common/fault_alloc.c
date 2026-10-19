/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-19 */
/* Failing allocator hook of the unit tests (EMAIL-139). */
#include "fault_alloc.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

/**
 * @file fault_alloc.c
 * @brief See fault_alloc.h.  The __wrap_ functions replace malloc, calloc and
 *        realloc in the test runner through the linker's --wrap option.
 */

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t m);
void *__real_realloc(void *p, size_t n);

static long g_countdown;    /* allocations left before the failing one; 0 = off */
static int  g_fired;

/** 1 when this allocation is the one to fail. */
static int should_fail(void)
{
    long left = __atomic_load_n(&g_countdown, __ATOMIC_RELAXED);
    while (left > 0) {
        if (__atomic_compare_exchange_n(&g_countdown, &left, left - 1, 0,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            if (left == 1) {
                __atomic_store_n(&g_fired, 1, __ATOMIC_RELAXED);
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

void fault_alloc_arm(long nth)
{
    __atomic_store_n(&g_fired, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_countdown, nth, __ATOMIC_RELAXED);
}

int fault_alloc_disarm(void)
{
    __atomic_store_n(&g_countdown, 0, __ATOMIC_RELAXED);
    return __atomic_load_n(&g_fired, __ATOMIC_RELAXED);
}

void *__wrap_malloc(size_t n)
{
    if (should_fail()) { errno = ENOMEM; return NULL; }
    return __real_malloc(n);
}

void *__wrap_calloc(size_t n, size_t m)
{
    if (should_fail()) { errno = ENOMEM; return NULL; }
    return __real_calloc(n, m);
}

void *__wrap_realloc(void *p, size_t n)
{
    if (should_fail()) { errno = ENOMEM; return NULL; }
    return __real_realloc(p, n);
}
