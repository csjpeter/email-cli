/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-29 */
/* Interface of the output medium the HTML renderer writes to. */
#ifndef HTML_MEDIUM_H
#define HTML_MEDIUM_H

#include <stdint.h>

/**
 * @file html_medium.h
 * @brief Target medium interface for the HTML renderer.
 *
 * The HTML renderer calls html_medium_char_width() to measure the
 * displayed column-width of Unicode codepoints.  The concrete
 * implementation is selected at compile/link time (platform/posix/
 * or tests/unit/html_medium_stub.c for unit tests).
 *
 * This follows the same pattern as platform/terminal.h: the header
 * declares the interface; CMake links in the right implementation.
 * No function pointers, no NULL risk, no runtime overhead.
 */

/**
 * @brief Returns the display column-width of a Unicode codepoint.
 *
 * @return 0  non-printable / control character
 *         1  narrow character
 *         2  wide character (CJK, etc.)
 */
int html_medium_char_width(uint32_t cp);

#endif /* HTML_MEDIUM_H */
