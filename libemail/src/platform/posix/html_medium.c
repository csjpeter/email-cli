/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-29 */
/* POSIX HTML render medium built on open_memstream. */
#include "html_medium.h"
#include <wchar.h>

int html_medium_char_width(uint32_t cp) {
    int w = wcwidth((wchar_t)cp);
    return (w < 0) ? 0 : w;
}
