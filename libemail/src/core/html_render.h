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

#endif /* HTML_RENDER_H */
