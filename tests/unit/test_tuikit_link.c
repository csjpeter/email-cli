/**
 * @file test_tuikit_link.c
 * @brief Proves libtuikit is linked statically into the build and behaves as
 *        the migration (EMAIL-28) will rely on.
 *
 * A static archive hands over only the objects something refers to, so a link
 * that is wired wrongly goes unnoticed until the first real use.  Calling the
 * library from the unit runner makes the wiring itself a tested fact.
 */
#include "test_helpers.h"
#include <tuikit/tuikit.h>
#include <locale.h>
#include <string.h>

void test_tuikit_link(void) {
    /* Widths come from the library's own table, not from the program's
     * locale: the same answers whatever setlocale() was or was not called. */
    ASSERT(tui_str_width("hello") == 5, "tuikit: ASCII is one column per char");
    ASSERT(tui_str_width("\xC3\xA1rv\xC3\xADzt\xC5\xB0r\xC5\x91") == 9,
           "tuikit: accented Hungarian letters are one column each");
    ASSERT(tui_str_width("\xE4\xB8\x96\xE7\x95\x8C") == 4,
           "tuikit: CJK ideographs are two columns each");

    /* The result must not depend on the locale: that is the capability the
     * program's own terminal_wcwidth() (glibc wcwidth) lacks in the "C" locale. */
    const char *saved = setlocale(LC_CTYPE, NULL);
    char keep[64];
    snprintf(keep, sizeof(keep), "%s", saved ? saved : "C");
    setlocale(LC_CTYPE, "C");
    ASSERT(tui_str_width("\xE4\xB8\x96") == 2,
           "tuikit: width of a wide character does not depend on LC_CTYPE");
    setlocale(LC_CTYPE, keep);

    /* A UTF-8 sequence cut short is not a character: the library answers
     * U+FFFD (replacement) rather than inventing one, and consumes only what
     * was there. */
    uint32_t cp = 0;
    int used = tui_utf8_decode("\xE4\xB8", 2, &cp);
    ASSERT(cp == 0xFFFD, "tuikit: a truncated UTF-8 sequence decodes to U+FFFD");
    ASSERT(used == 2, "tuikit: and consumes only the bytes that were present");
}
