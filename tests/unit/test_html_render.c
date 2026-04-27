#include "test_helpers.h"
#include "html_render.h"
#include <string.h>
#include <stdlib.h>

/* All tests use html_medium_stub (every printable codepoint = 1 column). */

/* ── Style-balance helpers ────────────────────────────────────────────── */

/** Count non-overlapping occurrences of needle in s. */
static int count_str(const char *s, const char *needle) {
    int n = 0;
    size_t nl = strlen(needle);
    for (const char *p = s; (p = strstr(p, needle)) != NULL; p += nl) n++;
    return n;
}

/**
 * Render html with ansi=1 and assert that every ANSI "style-on" escape is
 * paired with its "style-off" counterpart (equal occurrence counts).
 *
 * Checks: bold \033[1m/\033[22m, italic \033[3m/\033[23m,
 *         underline \033[4m/\033[24m, strikethrough \033[9m/\033[29m,
 *         fg color \033[38;2;/\033[39m, bg color \033[48;2;/\033[49m.
 */
static void assert_style_balanced(const char *html, const char *label) {
    char *r = html_render(html, 0, 1);
    if (!r) { ASSERT(0, label); return; }

    /* bold */
    int bold_on  = count_str(r, "\033[1m");
    int bold_off = count_str(r, "\033[22m");
    ASSERT(bold_on == bold_off,   label);

    /* italic */
    int ital_on  = count_str(r, "\033[3m");
    int ital_off = count_str(r, "\033[23m");
    ASSERT(ital_on == ital_off,   label);

    /* underline */
    int uline_on  = count_str(r, "\033[4m");
    int uline_off = count_str(r, "\033[24m");
    ASSERT(uline_on == uline_off, label);

    /* strikethrough */
    int strike_on  = count_str(r, "\033[9m");
    int strike_off = count_str(r, "\033[29m");
    ASSERT(strike_on == strike_off, label);

    /* fg color — both 24-bit (CSS) and named palette (URL blue) */
    int fg_on  = count_str(r, "\033[38;2;") + count_str(r, "\033[34m");
    int fg_off = count_str(r, "\033[39m");
    ASSERT(fg_on == fg_off,       label);

    /* bg color */
    int bg_on  = count_str(r, "\033[48;2;");
    int bg_off = count_str(r, "\033[49m");
    ASSERT(bg_on == bg_off,       label);

    free(r);
}

void test_html_render(void) {

    /* 1. NULL input → NULL */
    {
        char *r = html_render(NULL, 0, 0);
        ASSERT(r == NULL, "render NULL: returns NULL");
    }

    /* 2. Empty input → empty string */
    {
        char *r = html_render("", 0, 0);
        ASSERT(r != NULL, "render empty: not NULL");
        ASSERT(*r == '\0', "render empty: empty string");
        free(r);
    }

    /* 3. Plain text passthrough */
    {
        char *r = html_render("Hello World", 0, 0);
        ASSERT(r && strcmp(r, "Hello World") == 0, "plain text passthrough");
        free(r);
    }

    /* 4. <b>X</b> → ansi=1: bold escapes */
    {
        char *r = html_render("<b>X</b>", 0, 1);
        ASSERT(r != NULL, "bold ansi: not NULL");
        ASSERT(strstr(r, "\033[1m") != NULL, "bold ansi: bold on");
        ASSERT(strstr(r, "X") != NULL, "bold ansi: content");
        ASSERT(strstr(r, "\033[22m") != NULL, "bold ansi: bold off");
        free(r);
    }

    /* 5. <b>X</b> → ansi=0: no escapes, just X */
    {
        char *r = html_render("<b>X</b>", 0, 0);
        ASSERT(r && strcmp(r, "X") == 0, "bold plain: just X");
        free(r);
    }

    /* 6. <i>X</i> → ansi=1: italic escapes */
    {
        char *r = html_render("<i>X</i>", 0, 1);
        ASSERT(r && strstr(r, "\033[3m") && strstr(r, "\033[23m"),
               "italic ansi: escapes present");
        free(r);
    }

    /* 7. <u>X</u> → ansi=1: underline escapes */
    {
        char *r = html_render("<u>X</u>", 0, 1);
        ASSERT(r && strstr(r, "\033[4m") && strstr(r, "\033[24m"),
               "underline ansi: escapes present");
        free(r);
    }

    /* 8. <br> → \n */
    {
        char *r = html_render("<br>", 0, 0);
        ASSERT(r != NULL, "br: not NULL");
        ASSERT(strchr(r, '\n') != NULL, "br: contains newline");
        free(r);
    }

    /* 9. <p>A</p><p>B</p> → A\n\nB\n\n */
    {
        char *r = html_render("<p>A</p><p>B</p>", 0, 0);
        ASSERT(r != NULL, "p p: not NULL");
        ASSERT(strcmp(r, "A\n\nB\n\n") == 0, "p p: paragraph separation");
        free(r);
    }

    /* 10. <h1> bold + blank line (ansi=1) */
    {
        char *r = html_render("<h1>Title</h1>", 0, 1);
        ASSERT(r != NULL, "h1: not NULL");
        ASSERT(strstr(r, "\033[1m") != NULL, "h1: bold on");
        ASSERT(strstr(r, "Title") != NULL, "h1: title present");
        /* ends with \n\n */
        size_t len = strlen(r);
        ASSERT(len >= 2 && r[len-1] == '\n' && r[len-2] == '\n',
               "h1: ends with blank line");
        free(r);
    }

    /* 11. <ul><li>a</li><li>b</li></ul> → bullets */
    {
        char *r = html_render("<ul><li>a</li><li>b</li></ul>", 0, 0);
        ASSERT(r != NULL, "ul: not NULL");
        /* Should contain "a" and "b" on separate lines */
        ASSERT(strstr(r, "a") != NULL, "ul: contains a");
        ASSERT(strstr(r, "b") != NULL, "ul: contains b");
        /* Should contain bullet (U+2022 UTF-8: E2 80 A2) */
        ASSERT(strstr(r, "\xe2\x80\xa2") != NULL, "ul: contains bullet");
        free(r);
    }

    /* 12. <ol><li>a</li><li>b</li></ol> → numbered */
    {
        char *r = html_render("<ol><li>a</li><li>b</li></ol>", 0, 0);
        ASSERT(r != NULL, "ol: not NULL");
        ASSERT(strstr(r, "1.") != NULL, "ol: first item numbered");
        ASSERT(strstr(r, "2.") != NULL, "ol: second item numbered");
        ASSERT(strstr(r, "a") != NULL, "ol: item a");
        ASSERT(strstr(r, "b") != NULL, "ol: item b");
        free(r);
    }

    /* 13. <script> → empty output */
    {
        char *r = html_render("<script>js()</script>", 0, 0);
        ASSERT(r != NULL, "script: not NULL");
        ASSERT(*r == '\0', "script: empty output");
        free(r);
    }

    /* 14. <style> → empty output */
    {
        char *r = html_render("<style>body{color:red}</style>", 0, 0);
        ASSERT(r != NULL, "style: not NULL");
        ASSERT(*r == '\0', "style: empty output");
        free(r);
    }

    /* 15. Entities decoded in output */
    {
        char *r = html_render("a &amp; b", 0, 0);
        ASSERT(r && strcmp(r, "a & b") == 0, "entity in render: decoded");
        free(r);
    }

    /* 16. Word-wrap: width=10 → lines <= 10 chars */
    {
        char *r = html_render("Hello World Foo Bar Baz", 10, 0);
        ASSERT(r != NULL, "wrap: not NULL");
        /* Each line should be ≤ 10 chars */
        int ok = 1;
        const char *p = r;
        while (*p) {
            int linelen = 0;
            while (*p && *p != '\n') { linelen++; p++; }
            if (linelen > 10) { ok = 0; break; }
            if (*p == '\n') p++;
        }
        ASSERT(ok, "wrap width=10: all lines <= 10");
        free(r);
    }

    /* 17. width=0 → no line breaks from wrapping */
    {
        char *r = html_render("Hello World Foo Bar Baz", 0, 0);
        ASSERT(r != NULL, "no wrap: not NULL");
        ASSERT(strchr(r, '\n') == NULL, "no wrap: no newlines");
        free(r);
    }

    /* 18. <img alt="kep"> → [kep] */
    {
        char *r = html_render("<img alt=\"kep\">", 0, 0);
        ASSERT(r != NULL, "img alt: not NULL");
        ASSERT(strstr(r, "[kep]") != NULL, "img alt: [kep] in output");
        free(r);
    }

    /* 19. <a href="url">text</a> → link text + href URL emitted after */
    {
        char *r = html_render("<a href=\"http://example.com\">link text</a>", 0, 0);
        ASSERT(r != NULL, "a href: not NULL");
        ASSERT(strstr(r, "link text") != NULL, "a href: text present");
        ASSERT(strstr(r, "http://example.com") != NULL, "a href: URL present");
        free(r);
    }

    /* 20. <blockquote> → > prefix */
    {
        char *r = html_render("<blockquote>text</blockquote>", 0, 0);
        ASSERT(r != NULL, "blockquote: not NULL");
        ASSERT(strstr(r, "> text") != NULL || strstr(r, ">text") != NULL,
               "blockquote: > prefix present");
        free(r);
    }

    /* 21. <strong> same as <b> */
    {
        char *r = html_render("<strong>X</strong>", 0, 1);
        ASSERT(r && strstr(r, "\033[1m"), "strong: bold escape present");
        free(r);
    }

    /* 22. <em> same as <i> */
    {
        char *r = html_render("<em>X</em>", 0, 1);
        ASSERT(r && strstr(r, "\033[3m"), "em: italic escape present");
        free(r);
    }

    /* 23. Nested bold — only one pair of escapes */
    {
        char *r = html_render("<b><b>X</b></b>", 0, 1);
        ASSERT(r != NULL, "nested bold: not NULL");
        /* Count \033[1m occurrences: should be exactly 1 */
        int cnt = 0;
        const char *p = r;
        while ((p = strstr(p, "\033[1m")) != NULL) { cnt++; p += 4; }
        ASSERT(cnt == 1, "nested bold: only one bold-on escape");
        free(r);
    }

    /* 24. <s>X</s> → ansi=1: strikethrough escapes */
    {
        char *r = html_render("<s>X</s>", 0, 1);
        ASSERT(r != NULL, "strike: not NULL");
        ASSERT(strstr(r, "\033[9m") != NULL, "strike: strike-on escape");
        ASSERT(strstr(r, "\033[29m") != NULL, "strike: strike-off escape");
        free(r);
    }

    /* 25. <del>X</del> → same as <s> */
    {
        char *r = html_render("<del>X</del>", 0, 1);
        ASSERT(r != NULL, "del: not NULL");
        ASSERT(strstr(r, "\033[9m") != NULL, "del: strike-on escape");
        free(r);
    }

    /* 26. <h2>Title</h2> → bold + blank line */
    {
        char *r = html_render("<h2>Title</h2>", 0, 1);
        ASSERT(r != NULL, "h2: not NULL");
        ASSERT(strstr(r, "\033[1m") != NULL, "h2: bold on");
        size_t len = strlen(r);
        ASSERT(len >= 2 && r[len-1] == '\n' && r[len-2] == '\n',
               "h2: ends with blank line");
        free(r);
    }

    /* 27. <hr> → line of dashes */
    {
        char *r = html_render("<hr>", 0, 0);
        ASSERT(r != NULL, "hr: not NULL");
        ASSERT(strstr(r, "---") != NULL, "hr: contains dashes");
        free(r);
    }

    /* 28. <table><tr><td>A</td><td>B</td></tr></table> → A and B present */
    {
        char *r = html_render("<table><tr><td>A</td><td>B</td></tr></table>", 0, 0);
        ASSERT(r != NULL, "td: not NULL");
        ASSERT(strstr(r, "A") != NULL, "td: A present");
        ASSERT(strstr(r, "B") != NULL, "td: B present");
        free(r);
    }

    /* 29. <th>Head</th> → heading content present */
    {
        char *r = html_render("<table><tr><th>Head</th></tr></table>", 0, 0);
        ASSERT(r != NULL, "th: not NULL");
        ASSERT(strstr(r, "Head") != NULL, "th: heading present");
        free(r);
    }

    /* 30. <input value="val"> → value in output */
    {
        char *r = html_render("<input value=\"val\">", 0, 0);
        ASSERT(r != NULL, "input val: not NULL");
        ASSERT(strstr(r, "val") != NULL, "input val: value present");
        free(r);
    }

    /* 31. <pre> mode — no word-wrap, newlines preserved */
    {
        char *r = html_render("<pre>line1\nline2</pre>", 5, 0);
        ASSERT(r != NULL, "pre: not NULL");
        ASSERT(strstr(r, "line1") != NULL, "pre: line1 present");
        ASSERT(strstr(r, "line2") != NULL, "pre: line2 present");
        /* pre should not word-wrap even with width=5 */
        ASSERT(strstr(r, "line1\nline2") != NULL, "pre: newline preserved");
        free(r);
    }

    /* 32. Multi-byte UTF-8 word-wrap */
    {
        /* "árvíztűrő" is 9 chars, each 2-byte UTF-8 → 9 visible cols */
        char *r = html_render("\xc3\xa1rv\xc3\xadzt\xc5\xb1r\xc5\x91 world", 8, 0);
        ASSERT(r != NULL, "utf8 wrap: not NULL");
        /* must not crash and must contain both words */
        ASSERT(strstr(r, "world") != NULL, "utf8 wrap: world present");
        free(r);
    }

    /* 33. ANSI skip in visible width — ANSI escapes not counted as columns */
    {
        char *r = html_render("<b>Hello World Foo Bar</b>", 12, 1);
        ASSERT(r != NULL, "ansi vis_width: not NULL");
        /* Lines should still wrap at 12 visible columns despite ANSI escapes */
        int ok = 1;
        const char *p = r;
        while (*p) {
            int vis = 0;
            while (*p && *p != '\n') {
                if (*p == '\033') {
                    while (*p && *p != 'm') p++;
                    if (*p) p++;
                } else { vis++; p++; }
            }
            if (vis > 12) { ok = 0; break; }
            if (*p == '\n') p++;
        }
        ASSERT(ok, "ansi vis_width: wrap respects visible cols");
        free(r);
    }

    /* 34. style="font-weight:bold" → bold escape (parse_style) */
    {
        char *r = html_render("<span style=\"font-weight:bold\">X</span>", 0, 1);
        ASSERT(r != NULL, "style bold: not NULL");
        ASSERT(strstr(r, "\033[1m") != NULL, "style bold: bold escape present");
        free(r);
    }

    /* 35. style="font-style:italic" → italic escape */
    {
        char *r = html_render("<span style=\"font-style:italic\">X</span>", 0, 1);
        ASSERT(r != NULL, "style italic: not NULL");
        ASSERT(strstr(r, "\033[3m") != NULL, "style italic: italic escape present");
        free(r);
    }

    /* 36. style="text-decoration:underline" → underline escape */
    {
        char *r = html_render("<span style=\"text-decoration:underline\">X</span>", 0, 1);
        ASSERT(r != NULL, "style uline: not NULL");
        ASSERT(strstr(r, "\033[4m") != NULL, "style uline: underline escape present");
        free(r);
    }

    /* 37. style="color:#FF0000" → 24-bit ANSI color escape */
    {
        char *r = html_render("<span style=\"color:#FF0000\">X</span>", 0, 1);
        ASSERT(r != NULL, "style color hex6: not NULL");
        /* Should contain \033[38;2;255;0;0m */
        ASSERT(strstr(r, "\033[38;2;255;0;0m") != NULL, "style color #RRGGBB: escape present");
        free(r);
    }

    /* 38. style="color:#F00" → 24-bit ANSI (shorthand #RGB) */
    {
        char *r = html_render("<span style=\"color:#F00\">X</span>", 0, 1);
        ASSERT(r != NULL, "style color hex3: not NULL");
        ASSERT(strstr(r, "\033[38;2;") != NULL, "style color #RGB: escape present");
        free(r);
    }

    /* 39. style="color:red" → named CSS color → ANSI escape */
    {
        char *r = html_render("<span style=\"color:red\">X</span>", 0, 1);
        ASSERT(r != NULL, "style color named: not NULL");
        ASSERT(strstr(r, "\033[38;2;") != NULL, "style color name: escape present");
        free(r);
    }

    /* 40. style="background-color:blue" → bg colors suppressed */
    {
        char *r = html_render("<span style=\"background-color:blue\">X</span>", 0, 1);
        ASSERT(r != NULL, "style bgcolor: not NULL");
        ASSERT(strstr(r, "\033[48;2;") == NULL, "style bgcolor: bg escape suppressed");
        ASSERT(strstr(r, "X") != NULL, "style bgcolor: text still present");
        free(r);
    }

    /* 41. style="background-color:#0000FF" → bg colors suppressed */
    {
        char *r = html_render("<span style=\"background-color:#0000FF\">X</span>", 0, 1);
        ASSERT(r != NULL, "style bgcolor hex: not NULL");
        ASSERT(strstr(r, "\033[48;2;") == NULL, "style bgcolor hex: bg escape suppressed");
        ASSERT(strstr(r, "X") != NULL, "style bgcolor hex: text still present");
        free(r);
    }

    /* 42. 3-byte UTF-8 text in html_render (triggers utf8_adv 3-byte path) */
    {
        /* U+4E2D = \xe4\xb8\xad (3-byte), U+6587 = \xe6\x96\x87 (3-byte) */
        char *r = html_render("\xe4\xb8\xad\xe6\x96\x87 hello", 0, 0);
        ASSERT(r != NULL, "utf8 3byte: not NULL");
        ASSERT(strstr(r, "hello") != NULL, "utf8 3byte: ASCII present");
        free(r);
    }

    /* 43. 4-byte UTF-8 text (triggers utf8_adv 4-byte path) */
    {
        /* U+1F600 = \xf0\x9f\x98\x80 (4-byte emoji) */
        char *r = html_render("\xf0\x9f\x98\x80 hello", 0, 0);
        ASSERT(r != NULL, "utf8 4byte: not NULL");
        ASSERT(strstr(r, "hello") != NULL, "utf8 4byte: ASCII present");
        free(r);
    }

    /* 44. Invalid hex digit in CSS color (hex_val returns 0 for unknown char)
     *     #GGGGGG parses to black (0,0,0); max=0 < 160 → dark fg suppressed */
    {
        char *r = html_render("<span style=\"color:#GGGGGG\">X</span>", 0, 1);
        ASSERT(r != NULL, "hex_val invalid: not NULL");
        /* Black (0,0,0) is too dark — suppressed under color-filter policy */
        ASSERT(strstr(r, "\033[38;2;") == NULL, "hex_val invalid: dark color suppressed");
        ASSERT(strstr(r, "X") != NULL, "hex_val invalid: text still present");
        free(r);
    }

    /* 45. ANSI escape in img alt → str_vis_width ANSI skip path (lines 73-74) */
    {
        /* alt attribute contains an ANSI escape → str_vis_width must skip it */
        char *r = html_render("<img alt=\"\033[1mBold\033[0m\">", 0, 0);
        ASSERT(r != NULL, "ansi in img alt: not NULL");
        /* The alt text is rendered as [<alt>] — content is visible part */
        ASSERT(strstr(r, "Bold") != NULL, "ansi in img alt: text visible");
        free(r);
    }

    /* 46. <img alt=" "> (space only) → no [ ] rendered (is_blank_str) */
    {
        char *r = html_render("<img alt=\" \">", 0, 0);
        ASSERT(r != NULL, "img blank alt space: not NULL");
        ASSERT(strstr(r, "[") == NULL, "img blank alt space: no [ ] output");
        free(r);
    }

    /* 47. <img alt> with nbsp-only (U+00A0) → no [ ] rendered */
    {
        /* \xC2\xA0 = UTF-8 encoding of U+00A0 NON-BREAKING SPACE */
        char *r = html_render("<img alt=\"\xC2\xA0\">", 0, 0);
        ASSERT(r != NULL, "img nbsp alt: not NULL");
        ASSERT(strstr(r, "[") == NULL, "img nbsp alt: no [ ] output");
        free(r);
    }

    /* 48. <img alt> with zwnj-only (U+200C) → no [ ] rendered */
    {
        /* \xE2\x80\x8C = UTF-8 encoding of U+200C ZERO WIDTH NON-JOINER */
        char *r = html_render("<img alt=\"\xE2\x80\x8C\">", 0, 0);
        ASSERT(r != NULL, "img zwnj alt: not NULL");
        ASSERT(strstr(r, "[") == NULL, "img zwnj alt: no [ ] output");
        free(r);
    }

    /* 49. &zwnj; entity → invisible (zero-width), does not appear as text */
    {
        char *r = html_render("A&zwnj;B", 0, 0);
        ASSERT(r != NULL, "zwnj entity: not NULL");
        /* zwnj is U+200C, zero-width: text must contain A and B */
        ASSERT(strstr(r, "A") != NULL, "zwnj entity: A present");
        ASSERT(strstr(r, "B") != NULL, "zwnj entity: B present");
        /* must NOT contain literal "&zwnj;" */
        ASSERT(strstr(r, "&zwnj;") == NULL, "zwnj entity: not literal");
        free(r);
    }

    /* 50. compact_lines: many consecutive blank lines collapsed to one */
    {
        /* Many <div> / <tr> blocks in a row produce multiple blank lines */
        char *r = html_render(
            "<div>A</div><div></div><div></div><div></div>"
            "<div></div><div></div><div>B</div>", 0, 0);
        ASSERT(r != NULL, "compact: not NULL");
        ASSERT(strstr(r, "A") != NULL, "compact: A present");
        ASSERT(strstr(r, "B") != NULL, "compact: B present");
        /* Must not have more than one consecutive blank line between A and B */
        const char *a = strstr(r, "A");
        const char *b = strstr(r, "B");
        ASSERT(a && b && b > a, "compact: A before B");
        /* Count blank lines (consecutive \n\n) between A and B */
        int max_blanks = 0, cur_blanks = 0;
        for (const char *p = a; p < b; p++) {
            if (*p == '\n') { cur_blanks++; if (cur_blanks > max_blanks) max_blanks = cur_blanks; }
            else            { cur_blanks = 0; }
        }
        ASSERT(max_blanks <= 2, "compact: at most one blank line between blocks");
        free(r);
    }

    /* 51. compact_lines: paragraph spacing preserved (≤1 blank line kept) */
    {
        char *r = html_render("<p>A</p><p>B</p>", 0, 0);
        ASSERT(r != NULL, "compact para: not NULL");
        /* compact_lines must not strip intentional paragraph blank line */
        ASSERT(strstr(r, "A\n\nB") != NULL, "compact para: blank line preserved");
        free(r);
    }

    /* 52. <a style="text-decoration:underline"> must not bleed underline to
     *     subsequent text — parse_style depth counters must be balanced by
     *     traverse() even when tag_close has no handler for <a>. */
    {
        char *r = html_render(
            "<a style=\"text-decoration:underline\">link</a> normal", 0, 1);
        ASSERT(r != NULL, "style bleed: not NULL");
        /* The underline-off escape must appear after the link */
        ASSERT(strstr(r, "\033[24m") != NULL, "style bleed: underline closed");
        /* After the underline close, 'normal' must follow without underline-on */
        const char *off = strstr(r, "\033[24m");
        ASSERT(off != NULL && strstr(off, "normal") != NULL,
               "style bleed: 'normal' comes after underline-off");
        /* Must not re-open underline before 'normal' */
        const char *after_off = off + 5;  /* skip \033[24m */
        const char *uline_on = strstr(after_off, "\033[4m");
        const char *normal_pos = strstr(after_off, "normal");
        ASSERT(normal_pos != NULL, "style bleed: 'normal' found after off");
        ASSERT(uline_on == NULL || uline_on > normal_pos,
               "style bleed: no underline-on before 'normal'");
        free(r);
    }

    /* 53. <span style="font-weight:bold"> on non-<b> element — bold must
     *     be closed when </span> is processed. */
    {
        char *r = html_render(
            "<span style=\"font-weight:bold\">bold</span> plain", 0, 1);
        ASSERT(r != NULL, "span bold: not NULL");
        ASSERT(strstr(r, "\033[22m") != NULL, "span bold: bold closed");
        const char *off = strstr(r, "\033[22m");
        ASSERT(off && strstr(off, "plain") != NULL,
               "span bold: 'plain' after bold-off");
        free(r);
    }

    /* 55. compact_lines: trailing whitespace trimmed from lines */
    {
        /* <pre> preserves whitespace; inject spaces at end of a line */
        char *r = html_render("<pre>hello   \nworld</pre>", 0, 0);
        ASSERT(r != NULL, "compact trim: not NULL");
        /* trailing spaces on "hello   " line must be trimmed */
        ASSERT(strstr(r, "hello   ") == NULL, "compact trim: trailing spaces removed");
        ASSERT(strstr(r, "hello") != NULL,    "compact trim: hello still present");
        free(r);
    }

    /* 56. <a style="color:#FF0000"> must not bleed foreground color to
     *     subsequent text — apply_color depth counter balanced by traverse(). */
    {
        char *r = html_render(
            "<a style=\"color:#FF0000\">red link</a> normal", 0, 1);
        ASSERT(r != NULL, "color bleed fg: not NULL");
        /* Default-fg reset \033[39m must appear after the link */
        ASSERT(strstr(r, "\033[39m") != NULL, "color bleed fg: fg reset present");
        /* 'normal' must come after the reset */
        const char *reset = strstr(r, "\033[39m");
        ASSERT(reset && strstr(reset, "normal") != NULL,
               "color bleed fg: 'normal' after fg reset");
        free(r);
    }

    /* 57. <span style="background-color:#0000FF"> — bg suppressed,
     *     no bleed possible, 'after' renders in default colors. */
    {
        char *r = html_render(
            "<span style=\"background-color:#0000FF\">bg</span> after", 0, 1);
        ASSERT(r != NULL, "color bleed bg: not NULL");
        ASSERT(strstr(r, "\033[48;2;") == NULL, "color bleed bg: bg escape suppressed");
        ASSERT(strstr(r, "\033[49m")   == NULL, "color bleed bg: no bg reset needed");
        ASSERT(strstr(r, "after") != NULL, "color bleed bg: 'after' present");
        free(r);
    }
}

/* ── Style-balance comprehensive tests ───────────────────────────────── */

void test_html_render_style_balance(void) {

    /* ── Semantic tags — well-formed (with closing tag) ──────────────── */
    assert_style_balanced("<b>X</b> Y",              "b closed");
    assert_style_balanced("<strong>X</strong> Y",    "strong closed");
    assert_style_balanced("<i>X</i> Y",              "i closed");
    assert_style_balanced("<em>X</em> Y",            "em closed");
    assert_style_balanced("<u>X</u> Y",              "u closed");
    assert_style_balanced("<s>X</s> Y",              "s closed");
    assert_style_balanced("<del>X</del> Y",          "del closed");
    assert_style_balanced("<strike>X</strike> Y",    "strike closed");

    /* ── Semantic tags — missing closing tag ─────────────────────────── */
    assert_style_balanced("<b>X Y",                  "b unclosed");
    assert_style_balanced("<strong>X Y",             "strong unclosed");
    assert_style_balanced("<i>X Y",                  "i unclosed");
    assert_style_balanced("<em>X Y",                 "em unclosed");
    assert_style_balanced("<u>X Y",                  "u unclosed");
    assert_style_balanced("<s>X Y",                  "s unclosed");
    assert_style_balanced("<del>X Y",                "del unclosed");

    /* ── Inline style on <span> — well-formed ────────────────────────── */
    assert_style_balanced(
        "<span style=\"font-weight:bold\">X</span> Y",
        "span bold closed");
    assert_style_balanced(
        "<span style=\"font-style:italic\">X</span> Y",
        "span italic closed");
    assert_style_balanced(
        "<span style=\"text-decoration:underline\">X</span> Y",
        "span underline closed");
    assert_style_balanced(
        "<span style=\"color:#FF0000\">X</span> Y",
        "span color closed");
    assert_style_balanced(
        "<span style=\"background-color:#0000FF\">X</span> Y",
        "span bgcolor closed");

    /* ── Inline style on <span> — missing closing tag ────────────────── */
    assert_style_balanced(
        "<span style=\"font-weight:bold\">X Y",
        "span bold unclosed");
    assert_style_balanced(
        "<span style=\"font-style:italic\">X Y",
        "span italic unclosed");
    assert_style_balanced(
        "<span style=\"text-decoration:underline\">X Y",
        "span underline unclosed");
    assert_style_balanced(
        "<span style=\"color:#FF0000\">X Y",
        "span color unclosed");
    assert_style_balanced(
        "<span style=\"background-color:#0000FF\">X Y",
        "span bgcolor unclosed");

    /* ── Inline style on <a> (common in newsletters) ─────────────────── */
    assert_style_balanced(
        "<a href=\"x\" style=\"color:#333;text-decoration:underline\">link</a> Y",
        "a color+uline closed");
    assert_style_balanced(
        "<a href=\"x\" style=\"color:#333;text-decoration:underline\">link Y",
        "a color+uline unclosed");

    /* ── Inline style on non-inline elements (<td>, <div>) ───────────── */
    assert_style_balanced(
        "<td style=\"font-weight:bold;color:#FF0000\">X</td> Y",
        "td bold+color closed");
    assert_style_balanced(
        "<div style=\"font-style:italic;background-color:#EEE\">X</div> Y",
        "div italic+bgcolor closed");
    assert_style_balanced(
        "<div style=\"font-style:italic;background-color:#EEE\">X Y",
        "div italic+bgcolor unclosed");

    /* ── Multiple styles combined on one element ─────────────────────── */
    assert_style_balanced(
        "<span style=\"font-weight:bold;font-style:italic;"
        "text-decoration:underline;color:#FF0000;"
        "background-color:#0000FF\">X</span> Y",
        "span all-styles closed");
    assert_style_balanced(
        "<span style=\"font-weight:bold;font-style:italic;"
        "text-decoration:underline;color:#FF0000;"
        "background-color:#0000FF\">X Y",
        "span all-styles unclosed");

    /* ── Deeply nested, all unclosed ─────────────────────────────────── */
    assert_style_balanced(
        "<b><i><u><span style=\"color:red\">deep text",
        "deeply nested unclosed");

    /* ── Deeply nested, properly closed ──────────────────────────────── */
    assert_style_balanced(
        "<b><i><u><span style=\"color:red\">deep</span></u></i></b> Y",
        "deeply nested closed");

    /* ── Mixed: some closed, some not ────────────────────────────────── */
    assert_style_balanced(
        "<b>bold</b> <i>italic <u>both",
        "mixed partial unclosed");
    assert_style_balanced(
        "<span style=\"color:red\">A</span>"
        "<span style=\"color:blue\">B</span>"
        "<span style=\"color:green\">C</span>",
        "three color spans closed");
    assert_style_balanced(
        "<span style=\"color:red\">A"
        "<span style=\"color:blue\">B"
        "<span style=\"color:green\">C",
        "three color spans unclosed nested");

    /* ── Named CSS colors ────────────────────────────────────────────── */
    assert_style_balanced(
        "<span style=\"color:red\">X</span> Y",        "named color red");
    assert_style_balanced(
        "<span style=\"color:blue\">X</span> Y",       "named color blue");
    assert_style_balanced(
        "<span style=\"background-color:yellow\">X</span> Y",
        "named bgcolor yellow");

    /* ── #RGB shorthand ──────────────────────────────────────────────── */
    assert_style_balanced(
        "<span style=\"color:#F00\">X</span> Y",       "color #RGB closed");
    assert_style_balanced(
        "<span style=\"color:#F00\">X Y",              "color #RGB unclosed");

    /* ── Realistic newsletter snippet ────────────────────────────────── */
    assert_style_balanced(
        "<div style=\"background-color:#f4f4f4\">"
          "<table><tr>"
            "<td style=\"color:#333333;font-weight:bold\">Title</td>"
            "<td style=\"color:#999999\">"
              "<a style=\"color:#999;text-decoration:underline\""
              " href=\"x\">click here</a>"
            "</td>"
          "</tr></table>"
        "</div>",
        "newsletter snippet all closed");

    /* Same snippet with several closing tags omitted */
    assert_style_balanced(
        "<div style=\"background-color:#f4f4f4\">"
          "<table><tr>"
            "<td style=\"color:#333333;font-weight:bold\">Title"
            "<td style=\"color:#999999\">"
              "<a style=\"color:#999;text-decoration:underline\""
              " href=\"x\">click here",
        "newsletter snippet partial unclosed");
}

/* ── Parent-close forces child style reset ───────────────────────────── */

/**
 * Renders html with ansi=1, finds 'marker' in the output, then checks that
 * the output prefix (everything BEFORE marker) has balanced on/off ANSI
 * escape counts.  This proves that closing the parent tag closed all child
 * styles before the next sibling (marker) was written.
 *
 * HTML must be structured so that 'marker' appears as plain text AFTER the
 * parent's closing tag, e.g.:  <parent><child>...</parent>MARKER
 */
static void assert_style_closed_before(const char *html, const char *marker,
                                       const char *label)
{
    char *r = html_render(html, 0, 1);
    if (!r) { ASSERT(0, label); return; }

    const char *m = strstr(r, marker);
    if (!m) {
        /* marker not present in output — fail with context */
        ASSERT(0, label);
        free(r);
        return;
    }

    size_t prefix_len = (size_t)(m - r);
    char *prefix = malloc(prefix_len + 1);
    if (!prefix) { ASSERT(0, label); free(r); return; }
    memcpy(prefix, r, prefix_len);
    prefix[prefix_len] = '\0';

    int bold_on  = count_str(prefix, "\033[1m");
    int bold_off = count_str(prefix, "\033[22m");
    ASSERT(bold_on == bold_off, label);

    int ital_on  = count_str(prefix, "\033[3m");
    int ital_off = count_str(prefix, "\033[23m");
    ASSERT(ital_on == ital_off, label);

    int uline_on  = count_str(prefix, "\033[4m");
    int uline_off = count_str(prefix, "\033[24m");
    ASSERT(uline_on == uline_off, label);

    int strike_on  = count_str(prefix, "\033[9m");
    int strike_off = count_str(prefix, "\033[29m");
    ASSERT(strike_on == strike_off, label);

    int fg_on  = count_str(prefix, "\033[38;2;") + count_str(prefix, "\033[34m");
    int fg_off = count_str(prefix, "\033[39m");
    ASSERT(fg_on == fg_off, label);

    int bg_on  = count_str(prefix, "\033[48;2;");
    int bg_off = count_str(prefix, "\033[49m");
    ASSERT(bg_on == bg_off, label);

    free(prefix);
    free(r);
}

void test_html_render_parent_close(void)
{
    /* Marker text appended as sibling after the parent's closing tag.
     * traverse() visits parent (with snapshot/restore), then visits MARKER.
     * All child styles must be closed BEFORE MARKER is emitted. */

#define MK "XMARKERX"

    /* ── <div> parent — single unclosed child style ───────────────────── */
    assert_style_closed_before(
        "<div><b>bold text</div>" MK,
        MK, "div > b unclosed");
    assert_style_closed_before(
        "<div><i>italic text</div>" MK,
        MK, "div > i unclosed");
    assert_style_closed_before(
        "<div><u>underline text</div>" MK,
        MK, "div > u unclosed");
    assert_style_closed_before(
        "<div><s>strike text</div>" MK,
        MK, "div > s unclosed");
    assert_style_closed_before(
        "<div><strong>strong text</div>" MK,
        MK, "div > strong unclosed");
    assert_style_closed_before(
        "<div><em>em text</div>" MK,
        MK, "div > em unclosed");

    /* ── <div> parent — inline-style child (non-semantic) ────────────── */
    assert_style_closed_before(
        "<div><span style=\"color:#FF0000\">red</div>" MK,
        MK, "div > span color unclosed");
    assert_style_closed_before(
        "<div><span style=\"background-color:#0000FF\">bg</div>" MK,
        MK, "div > span bgcolor unclosed");
    assert_style_closed_before(
        "<div><span style=\"font-weight:bold\">bold</div>" MK,
        MK, "div > span bold unclosed");
    assert_style_closed_before(
        "<div><span style=\"font-style:italic\">ital</div>" MK,
        MK, "div > span italic unclosed");
    assert_style_closed_before(
        "<div><span style=\"text-decoration:underline\">uline</div>" MK,
        MK, "div > span uline unclosed");

    /* ── <p> parent ───────────────────────────────────────────────────── */
    assert_style_closed_before(
        "<p><b>bold</p>" MK,
        MK, "p > b unclosed");
    assert_style_closed_before(
        "<p><span style=\"color:red\">red</p>" MK,
        MK, "p > span color unclosed");

    /* ── <ul>/<li> parent ─────────────────────────────────────────────── */
    assert_style_closed_before(
        "<ul><li><b>bold item</li></ul>" MK,
        MK, "ul > li > b closed");
    assert_style_closed_before(
        "<ul><li><i>italic item</ul>" MK,
        MK, "ul > li > i unclosed");
    assert_style_closed_before(
        "<ul><li><span style=\"color:#F00\">colored</ul>" MK,
        MK, "ul > li > span color unclosed");

    /* ── <ol> parent ──────────────────────────────────────────────────── */
    assert_style_closed_before(
        "<ol><li><u>underline item</li></ol>" MK,
        MK, "ol > li > u closed");

    /* ── <blockquote> parent ──────────────────────────────────────────── */
    assert_style_closed_before(
        "<blockquote><b>bold quote</blockquote>" MK,
        MK, "blockquote > b unclosed");
    assert_style_closed_before(
        "<blockquote><span style=\"color:blue\">blue</blockquote>" MK,
        MK, "blockquote > span color unclosed");

    /* ── <table>/<tr>/<td> chain ──────────────────────────────────────── */
    assert_style_closed_before(
        "<table><tr><td><b>bold cell</td></tr></table>" MK,
        MK, "table > tr > td > b closed");
    assert_style_closed_before(
        "<table><tr><td style=\"color:#333\"><b>styled</td></tr></table>" MK,
        MK, "table > tr > td style+b closed");
    assert_style_closed_before(
        "<table><tr><td style=\"font-weight:bold\">bold cell</table>" MK,
        MK, "table > td style bold unclosed");
    assert_style_closed_before(
        "<table><tr>"
          "<td style=\"color:#333333\"><b>A</td>"
          "<td style=\"color:#999999\"><i>B</td>"
        "</tr></table>" MK,
        MK, "table multi-td style unclosed");

    /* ── <div> parent — well-formed child (with closing tag) ─────────── */
    assert_style_closed_before(
        "<div><b>bold</b></div>" MK,
        MK, "div > b closed");
    assert_style_closed_before(
        "<div><span style=\"color:#FF0000\">red</span></div>" MK,
        MK, "div > span color closed");

    /* ── Multiple nested unclosed children ───────────────────────────── */
    assert_style_closed_before(
        "<div><b><i><u>triple nested</div>" MK,
        MK, "div > b>i>u all unclosed");
    assert_style_closed_before(
        "<div><b><i><u>"
          "<span style=\"color:red;background-color:#00F\">deep</div>" MK,
        MK, "div > deep nested all styles unclosed");

    /* ── <a> with inline style ────────────────────────────────────────── */
    assert_style_closed_before(
        "<div>"
          "<a style=\"color:#333;text-decoration:underline\" href=\"x\">link"
        "</div>" MK,
        MK, "div > a color+uline unclosed");

    /* ── All styles combined on single child ─────────────────────────── */
    assert_style_closed_before(
        "<div>"
          "<span style=\"font-weight:bold;font-style:italic;"
                        "text-decoration:underline;"
                        "color:#FF0000;background-color:#0000FF\">all"
        "</div>" MK,
        MK, "div > span all-styles unclosed");

    /* ── Nested divs: outer close should reset everything ────────────── */
    assert_style_closed_before(
        "<div>"
          "<div>"
            "<b><i><span style=\"color:red\">deep</span></i></b>"
          "</div>"
        "</div>" MK,
        MK, "nested divs properly closed");
    assert_style_closed_before(
        "<div>"
          "<div>"
            "<b><i><span style=\"color:red\">deep"
          "</div>"       /* no closing for inner div, b, i, span */
        "</div>" MK,
        MK, "nested divs outer close resets all");

    /* ── Realistic newsletter: div wrapper > table > td with styles ───── */
    assert_style_closed_before(
        "<div style=\"background-color:#f4f4f4\">"
          "<table><tr>"
            "<td style=\"color:#333333;font-weight:bold\">Title"
            "<td style=\"color:#999999\">"
              "<a style=\"color:#999;text-decoration:underline\""
              " href=\"x\">click here"
          "</tr></table>"
        "</div>" MK,
        MK, "newsletter all styles reset before marker");

#undef MK
}

/* ── Color filtering tests ────────────────────────────────────────────
 *
 * Policy:
 *   - Background colors  → suppressed entirely (no \033[48;2; emitted)
 *   - Dark fg colors     → suppressed (max(r,g,b) < 160)
 *   - Bright fg colors   → allowed  (max(r,g,b) >= 160)
 *
 * Written BEFORE the fix so they define the expected behaviour.
 * ─────────────────────────────────────────────────────────────────── */

void test_html_render_color_filter(void)
{
    /* ── Background colors: never emitted ─────────────────────────── */

    /* Named bg color */
    {
        char *r = html_render(
            "<span style=\"background-color:blue\">X</span>", 0, 1);
        ASSERT(r != NULL, "bg suppress named: not NULL");
        ASSERT(strstr(r, "\033[48;2;") == NULL,
               "bg suppress named: no bg escape emitted");
        free(r);
    }

    /* Hex #RRGGBB bg color */
    {
        char *r = html_render(
            "<span style=\"background-color:#FF0000\">X</span>", 0, 1);
        ASSERT(r != NULL, "bg suppress hex6: not NULL");
        ASSERT(strstr(r, "\033[48;2;") == NULL,
               "bg suppress hex6: no bg escape emitted");
        free(r);
    }

    /* Hex #RGB shorthand bg color */
    {
        char *r = html_render(
            "<span style=\"background-color:#0F0\">X</span>", 0, 1);
        ASSERT(r != NULL, "bg suppress hex3: not NULL");
        ASSERT(strstr(r, "\033[48;2;") == NULL,
               "bg suppress hex3: no bg escape emitted");
        free(r);
    }

    /* bg-color suppressed → no bg-reset \033[49m either */
    {
        char *r = html_render(
            "<span style=\"background-color:white\">X</span> Y", 0, 1);
        ASSERT(r != NULL, "bg suppress reset: not NULL");
        ASSERT(strstr(r, "\033[48;2;") == NULL,
               "bg suppress reset: no bg-open escape");
        ASSERT(strstr(r, "\033[49m") == NULL,
               "bg suppress reset: no bg-reset escape");
        free(r);
    }

    /* ansi=0: bg color must also be absent (already was, stays so) */
    {
        char *r = html_render(
            "<span style=\"background-color:red\">X</span>", 0, 0);
        ASSERT(r != NULL && strcmp(r, "X") == 0,
               "bg ansi0: plain text only");
        free(r);
    }

    /* ── Dark foreground colors: suppressed (max(r,g,b) < 160) ────── */

    /* #333333 dark gray (max=51) */
    {
        char *r = html_render(
            "<span style=\"color:#333333\">text</span>", 0, 1);
        ASSERT(r != NULL, "dark fg #333: not NULL");
        ASSERT(strstr(r, "\033[38;2;") == NULL,
               "dark fg #333: no fg escape emitted");
        free(r);
    }

    /* #666666 medium dark gray (max=102) */
    {
        char *r = html_render(
            "<span style=\"color:#666666\">text</span>", 0, 1);
        ASSERT(r != NULL, "dark fg #666: not NULL");
        ASSERT(strstr(r, "\033[38;2;") == NULL,
               "dark fg #666: no fg escape emitted");
        free(r);
    }

    /* #808080 gray (max=128) — user said this is too dark */
    {
        char *r = html_render(
            "<span style=\"color:#808080\">text</span>", 0, 1);
        ASSERT(r != NULL, "dark fg gray: not NULL");
        ASSERT(strstr(r, "\033[38;2;") == NULL,
               "dark fg gray: no fg escape emitted");
        free(r);
    }

    /* CSS named 'gray' (#808080) */
    {
        char *r = html_render(
            "<span style=\"color:gray\">text</span>", 0, 1);
        ASSERT(r != NULL, "dark fg named gray: not NULL");
        ASSERT(strstr(r, "\033[38;2;") == NULL,
               "dark fg named gray: no fg escape emitted");
        free(r);
    }

    /* Dark navy #000080 (max=128) */
    {
        char *r = html_render(
            "<span style=\"color:#000080\">text</span>", 0, 1);
        ASSERT(r != NULL, "dark fg navy: not NULL");
        ASSERT(strstr(r, "\033[38;2;") == NULL,
               "dark fg navy: no fg escape emitted");
        free(r);
    }

    /* Dark suppressed → no fg-reset \033[39m either */
    {
        char *r = html_render(
            "<span style=\"color:#333\">X</span> Y", 0, 1);
        ASSERT(r != NULL, "dark fg reset: not NULL");
        ASSERT(strstr(r, "\033[38;2;") == NULL,
               "dark fg reset: no fg-open escape");
        ASSERT(strstr(r, "\033[39m") == NULL,
               "dark fg reset: no fg-reset escape");
        free(r);
    }

    /* Dark color text must still appear */
    {
        char *r = html_render(
            "<span style=\"color:#333333\">hello</span>", 0, 1);
        ASSERT(r && strstr(r, "hello") != NULL,
               "dark fg text: text still present");
        free(r);
    }

    /* ── Bright foreground colors: allowed (max(r,g,b) >= 160) ─────── */

    /* White #FFFFFF (max=255) */
    {
        char *r = html_render(
            "<span style=\"color:white\">X</span>", 0, 1);
        ASSERT(r != NULL, "bright fg white: not NULL");
        ASSERT(strstr(r, "\033[38;2;") != NULL,
               "bright fg white: fg escape emitted");
        free(r);
    }

    /* Bright red #FF0000 (max=255) */
    {
        char *r = html_render(
            "<span style=\"color:#FF0000\">X</span>", 0, 1);
        ASSERT(r != NULL, "bright fg red: not NULL");
        ASSERT(strstr(r, "\033[38;2;255;0;0m") != NULL,
               "bright fg red: correct escape emitted");
        free(r);
    }

    /* CSS 'red' (#FF0000, max=255) */
    {
        char *r = html_render(
            "<span style=\"color:red\">X</span>", 0, 1);
        ASSERT(r != NULL, "bright fg named red: not NULL");
        ASSERT(strstr(r, "\033[38;2;") != NULL,
               "bright fg named red: fg escape emitted");
        free(r);
    }

    /* #0000CC dark-ish blue (max=204 >= 160) */
    {
        char *r = html_render(
            "<span style=\"color:#0000CC\">X</span>", 0, 1);
        ASSERT(r != NULL, "bright fg blue CC: not NULL");
        ASSERT(strstr(r, "\033[38;2;") != NULL,
               "bright fg blue CC: fg escape emitted");
        free(r);
    }

    /* Bright fg → fg-reset must also be present (to close the span) */
    {
        char *r = html_render(
            "<span style=\"color:#FF0000\">X</span> Y", 0, 1);
        ASSERT(r != NULL, "bright fg reset: not NULL");
        ASSERT(strstr(r, "\033[38;2;") != NULL,
               "bright fg reset: fg-open present");
        ASSERT(strstr(r, "\033[39m") != NULL,
               "bright fg reset: fg-reset present");
        free(r);
    }

    /* ── Style-balance still holds after filtering ──────────────────── */
    /* (bg suppressed → both bg_on=0 and bg_off=0, trivially balanced)  */
    assert_style_balanced(
        "<span style=\"background-color:#FF0000\">X</span> Y",
        "bg filtered: still balanced");
    assert_style_balanced(
        "<div style=\"background-color:#f4f4f4\">"
          "<span style=\"color:#333\">dark text</span>"
        "</div>",
        "bg+dark fg filtered: still balanced");
    assert_style_balanced(
        "<span style=\"color:#FF0000\">bright</span>"
        "<span style=\"color:#808080\">dark</span>",
        "bright+dark fg mix: still balanced");
}

/* ── URL isolation ─────────────────────────────────────────────────────── */

void test_html_render_url_isolation(void)
{
#define URL "https://example.com/path"
#define URL2 "http://other.org/x"

    /* 1. URL mid-text: must start on its own line */
    {
        char *r = html_render("before " URL " after", 80, 0);
        ASSERT(r != NULL, "url mid: not NULL");
        const char *u = strstr(r, URL);
        ASSERT(u != NULL, "url mid: URL present");
        ASSERT(u == r || *(u - 1) == '\n', "url mid: URL starts after newline");
        const char *a = u + strlen(URL);
        ASSERT(*a == '\n' || *a == '\0', "url mid: URL ends with newline");
        free(r);
    }

    /* 2. URL at start of text: no spurious leading blank line */
    {
        char *r = html_render(URL " after", 80, 0);
        ASSERT(r != NULL, "url start: not NULL");
        const char *u = strstr(r, URL);
        ASSERT(u != NULL, "url start: URL present");
        /* URL is at start → either r itself or after '\n' */
        ASSERT(u == r || *(u - 1) == '\n', "url start: URL at start or after newline");
        const char *a = u + strlen(URL);
        ASSERT(*a == '\n' || *a == '\0', "url start: URL followed by newline");
        /* following text appears after the URL */
        ASSERT(strstr(a, "after") != NULL || strstr(r, "after") > u,
               "url start: text after URL is present");
        free(r);
    }

    /* 3. URL at end of text: just the URL, nothing after */
    {
        char *r = html_render("before " URL, 80, 0);
        ASSERT(r != NULL, "url end: not NULL");
        const char *u = strstr(r, URL);
        ASSERT(u != NULL, "url end: URL present");
        ASSERT(u == r || *(u - 1) == '\n', "url end: URL on own line");
        free(r);
    }

    /* 4. http:// scheme also isolated */
    {
        char *r = html_render("visit " URL2 " now", 80, 0);
        ASSERT(r != NULL, "http url: not NULL");
        const char *u = strstr(r, URL2);
        ASSERT(u != NULL, "http url: URL present");
        ASSERT(u == r || *(u - 1) == '\n', "http url: starts on own line");
        free(r);
    }

    /* 5. Two adjacent URLs each on their own line */
    {
        char *r = html_render(URL " " URL2, 80, 0);
        ASSERT(r != NULL, "two urls: not NULL");
        const char *u1 = strstr(r, URL);
        const char *u2 = strstr(r, URL2);
        ASSERT(u1 != NULL && u2 != NULL, "two urls: both present");
        ASSERT(u1 < u2, "two urls: first before second");
        /* There must be a newline between them */
        char between[64] = {0};
        size_t gap = (size_t)(u2 - (u1 + strlen(URL)));
        if (gap < sizeof(between)) {
            memcpy(between, u1 + strlen(URL), gap);
            between[gap] = '\0';
        }
        ASSERT(strchr(between, '\n') != NULL, "two urls: newline between them");
        free(r);
    }

    /* 6. URL inside <p> tag: same isolation rules apply */
    {
        char *r = html_render("<p>see " URL " for details</p>", 80, 0);
        ASSERT(r != NULL, "url in p: not NULL");
        const char *u = strstr(r, URL);
        ASSERT(u != NULL, "url in p: URL present");
        ASSERT(u == r || *(u - 1) == '\n', "url in p: URL on own line");
        const char *a = u + strlen(URL);
        ASSERT(*a == '\n' || *a == '\0', "url in p: followed by newline");
        free(r);
    }

    /* 7. Style-balance is preserved when URL isolation adds newlines */
    assert_style_balanced("<b>bold " URL " text</b>", "url isolation: style balanced");

/* Helper: check condition without early return (safe even if ASSERT would abort). */
#define CHECK(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
        g_tests_failed++; \
    } \
} while (0)

    /* 8. <a href="...">text</a>: href URL must appear in output */
    {
        char *r = html_render("<a href=\"" URL "\">Click here</a>", 80, 0);
        ASSERT(r != NULL, "anchor href: not NULL");
        CHECK(strstr(r, "Click here") != NULL, "anchor href: link text present");
        CHECK(strstr(r, URL) != NULL,           "anchor href: URL present in output");
        const char *u = strstr(r, URL);
        if (u) CHECK(u == r || *(u-1) == '\n',  "anchor href: URL on own line");
        free(r);
    }

    /* 9. <a href="#section">skip</a>: fragment-only href not emitted */
    {
        char *r = html_render("<a href=\"#section\">skip</a>", 80, 0);
        ASSERT(r != NULL, "anchor fragment: not NULL");
        CHECK(strstr(r, "#section") == NULL, "anchor fragment: not emitted");
        free(r);
    }

    /* 10. <a href="javascript:void(0)">skip</a>: js href not emitted */
    {
        char *r = html_render("<a href=\"javascript:void(0)\">skip</a>", 80, 0);
        ASSERT(r != NULL, "anchor js: not NULL");
        CHECK(strstr(r, "javascript:") == NULL, "anchor js: not emitted");
        free(r);
    }

#undef CHECK

#undef URL
#undef URL2
}

void test_html_render_url_color(void) {
    /* Plain-text URL in ANSI mode must be wrapped in blue (ESC[34m / ESC[39m) */
    {
        char *r = html_render("https://example.com/path", 80, 1);
        ASSERT(r != NULL, "url color: not NULL");
        ASSERT(strstr(r, "\033[34m") != NULL, "url color: opens blue");
        ASSERT(strstr(r, "\033[39m") != NULL, "url color: closes fg");
        ASSERT(strstr(r, "https://example.com/path") != NULL, "url color: URL present");
        free(r);
    }

    /* No ANSI in non-ANSI mode */
    {
        char *r = html_render("https://example.com/path", 80, 0);
        ASSERT(r != NULL, "url nocolor: not NULL");
        ASSERT(strstr(r, "\033[34m") == NULL, "url nocolor: no blue escape");
        free(r);
    }

    /* <a href> URL rendered blue */
    {
        char *r = html_render("<a href=\"https://click.example.com\">Click</a>", 80, 1);
        ASSERT(r != NULL, "url href color: not NULL");
        ASSERT(strstr(r, "\033[34m") != NULL, "url href color: opens blue");
        ASSERT(strstr(r, "https://click.example.com") != NULL, "url href color: URL present");
        free(r);
    }
}
