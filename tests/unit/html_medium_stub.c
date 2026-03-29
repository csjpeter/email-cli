#include "html_medium.h"

/* Deterministic stub for unit tests: every printable codepoint = 1 column. */
int html_medium_char_width(uint32_t cp) {
    if (cp < 0x20 || cp == 0x7F) return 0;
    return 1;
}
