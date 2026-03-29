#include "test_helpers.h"
#include "html_render.h"
#include <string.h>
#include <stdlib.h>

/* All tests use html_medium_stub (every printable codepoint = 1 column). */

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

    /* 19. <a href="url">text</a> → text (no URL in output) */
    {
        char *r = html_render("<a href=\"http://example.com\">link text</a>", 0, 0);
        ASSERT(r != NULL, "a href: not NULL");
        ASSERT(strstr(r, "link text") != NULL, "a href: text present");
        ASSERT(strstr(r, "http://") == NULL, "a href: URL not in output");
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

    /* 40. style="background-color:blue" → bg ANSI escape */
    {
        char *r = html_render("<span style=\"background-color:blue\">X</span>", 0, 1);
        ASSERT(r != NULL, "style bgcolor: not NULL");
        ASSERT(strstr(r, "\033[48;2;") != NULL, "style bgcolor: bg escape present");
        free(r);
    }

    /* 41. style="background-color:#0000FF" → bg ANSI escape */
    {
        char *r = html_render("<span style=\"background-color:#0000FF\">X</span>", 0, 1);
        ASSERT(r != NULL, "style bgcolor hex: not NULL");
        ASSERT(strstr(r, "\033[48;2;0;0;255m") != NULL, "style bgcolor hex: escape present");
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

    /* 44. Invalid hex digit in CSS color (hex_val returns 0 for unknown char) */
    {
        /* 'G' is not valid hex → hex_val('G')=0, r=g=b=0 → black */
        char *r = html_render("<span style=\"color:#GGGGGG\">X</span>", 0, 1);
        ASSERT(r != NULL, "hex_val invalid: not NULL");
        /* Should emit black color code (0,0,0) */
        ASSERT(strstr(r, "\033[38;2;0;0;0m") != NULL, "hex_val invalid: black color");
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
}
