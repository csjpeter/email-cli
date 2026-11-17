/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-03-29 */
/* Interface of the HTML to text renderer. */
#ifndef HTML_RENDER_H
#define HTML_RENDER_H

/**
 * @file html_render.h
 * @brief HTML → terminal/plain-text renderer.
 *
 * Character-width measurement is provided at link time by the
 * html_medium_char_width() function (see html_medium.h).
 * No function pointers, no runtime configuration of the medium.
 */

/**
 * @brief Renders an HTML string to a heap-allocated plain/ANSI string.
 *
 * @param html   Input HTML (UTF-8).  NULL → returns NULL.
 * @param width  Wrap column width; 0 = no wrapping.
 * @param ansi   1 = emit ANSI colour/style escapes; 0 = plain text.
 * @return Heap-allocated NUL-terminated string.  Caller must free().
 *         Returns NULL on OOM.
 */
char *html_render(const char *html, int width, int ansi);

/**
 * @brief Make message-derived text safe to print on a terminal (in place).
 *
 * C0 controls except '\n' and '\t', DEL and the C1 controls (U+0080-009F, as
 * UTF-8) are replaced by '?'; a '\r' directly before '\n' is dropped.  The
 * result is never longer than the input.
 *
 * @param s  NUL-terminated string, may be NULL.
 * @return s.
 */
char *html_sanitize_display(char *s);

#endif /* HTML_RENDER_H */
