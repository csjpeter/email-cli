/**
 * @file input_line_harness.c
 * @brief Minimal test harness for InputLine + PathComplete PTY-based tests.
 *
 * Usage: input-line-harness [initial_path]
 *
 * Enters raw mode, runs input_line_run() at row 1 with "Path: " prompt.
 * On Enter  → prints "RESULT: <buf>\n" to stdout.
 * On ESC    → prints "CANCELLED\n" to stdout.
 */

#define _DEFAULT_SOURCE

#include "input_line.h"
#include "path_complete.h"
#include "terminal.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
    const char *initial = (argc >= 2) ? argv[1] : "";

    TermRawState *raw = terminal_raw_enter();
    if (!raw) {
        fprintf(stderr, "terminal_raw_enter failed\n");
        return 1;
    }

    /* Clear screen, hide cursor, move to top-left */
    printf("\033[2J\033[H\033[?25l");
    fflush(stdout);

    char buf[4096];
    strncpy(buf, initial, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    InputLine il;
    input_line_init(&il, buf, sizeof(buf), buf);
    path_complete_attach(&il);

    int ok = input_line_run(&il, 1, "Path: ");

    path_complete_reset();
    terminal_raw_exit(&raw);

    /* Move past the input area before printing result */
    printf("\033[3;1H\033[?25l");

    if (ok)
        printf("RESULT: %s\n", il.buf);
    else
        printf("CANCELLED\n");

    fflush(stdout);
    return 0;
}
