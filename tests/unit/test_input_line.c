#include "test_helpers.h"
#include "input_line.h"
#include <string.h>
#include <stddef.h>

void test_input_line(void) {
    char buf[64];

    /* 1. Init with NULL initial_text → empty buffer, cursor at 0 */
    InputLine il;
    input_line_init(&il, buf, sizeof(buf), NULL);
    ASSERT(il.buf   == buf,       "buf pointer set");
    ASSERT(il.bufsz == sizeof(buf), "bufsz set");
    ASSERT(il.len   == 0,         "len 0 for NULL initial");
    ASSERT(il.cur   == 0,         "cur 0 for NULL initial");
    ASSERT(buf[0]   == '\0',      "buf NUL-terminated");

    /* 2. Init with empty string → same as NULL */
    input_line_init(&il, buf, sizeof(buf), "");
    ASSERT(il.len == 0, "len 0 for empty initial");
    ASSERT(il.cur == 0, "cur 0 for empty initial");

    /* 3. Init with text → len and cur at end */
    input_line_init(&il, buf, sizeof(buf), "hello");
    ASSERT(il.len == 5,           "len 5 for 'hello'");
    ASSERT(il.cur == 5,           "cur at end after init");
    ASSERT(strcmp(buf, "hello") == 0, "buf contains 'hello'");

    /* 4. Init truncates when text >= bufsz */
    char small[4];
    input_line_init(&il, small, sizeof(small), "toolong");
    ASSERT(il.len == 3,         "len truncated to bufsz-1");
    ASSERT(small[3] == '\0',    "buf NUL-terminated after truncation");

    /* 5. All callbacks NULL after init */
    input_line_init(&il, buf, sizeof(buf), "x");
    ASSERT(il.tab_fn       == NULL, "tab_fn NULL after init");
    ASSERT(il.shift_tab_fn == NULL, "shift_tab_fn NULL after init");
    ASSERT(il.render_below == NULL, "render_below NULL after init");

    /* 6. trow is 0 after init */
    ASSERT(il.trow == 0, "trow 0 after init");

    /* 7. UTF-8 multi-byte initial text */
    input_line_init(&il, buf, sizeof(buf), "héllo");
    /* 'é' is 2 bytes (0xC3 0xA9) → len = 6, cur = 6 */
    ASSERT(il.len == 6, "len accounts for UTF-8 bytes");
    ASSERT(il.cur == 6, "cur at byte end for UTF-8 text");
    ASSERT(strcmp(buf, "héllo") == 0, "UTF-8 content preserved");
}
