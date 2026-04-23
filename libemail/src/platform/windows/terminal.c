/* Windows terminal implementation — to be implemented for MinGW-w64 */
/* Uses Windows Console API: GetConsoleMode, SetConsoleMode,
 * GetConsoleScreenBufferInfo, ReadConsoleInput */
#include "../terminal.h"
#include <stdio.h>
#include <stdlib.h>

struct TermRawState {
    int placeholder;
};

int terminal_cols(void)                      { return 80; }
int terminal_rows(void)                      { return 0; }
int terminal_is_tty(int fd)                  { (void)fd; return 0; }
TermRawState *terminal_raw_enter(void)       { return NULL; }
void terminal_raw_exit(TermRawState **s)     { (void)s; }
TermKey terminal_read_key(void)              { return TERM_KEY_QUIT; }
int         terminal_last_printable(void)    { return 0; }
const char *terminal_last_utf8(void)         { return ""; }
int terminal_wcwidth(uint32_t cp)            { (void)cp; return 1; }
int terminal_read_password(const char *p, char *b, size_t n) {
    (void)p; (void)b; (void)n; return -1;
}
