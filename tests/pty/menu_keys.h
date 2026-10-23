/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-22 */
/* Reads the quick keys of the ':' action menu off the screen (EMAIL-225). */
#ifndef TESTS_PTY_MENU_KEYS_H
#define TESTS_PTY_MENU_KEYS_H

/**
 * @file menu_keys.h
 * @brief What the PTY tests of EMAIL-225 need to see of an open ':' menu: its
 *        rows, each with the quick key drawn at the left of the entry.
 *
 * A row of the menu is drawn as "L  key  Action - description", L being the
 * quick key (libtuikit, TUI-62).  The reader finds the box under the title
 * "Actions", takes the text after the left border of each row and splits off
 * the letter.  The tests include this header after ptytest.h and pty_assert.h.
 */

#include <stdio.h>
#include <string.h>

#define MENU_MAX_ROWS 40

typedef struct {
    char letter;        /**< the quick key, 0 when the row has none */
    char text[128];     /**< the rest of the row: key column, action, description */
} MenuRow;

/** The border character of the box, as UTF-8. */
#define MENU_BORDER "\xe2\x94\x82"

/**
 * @brief Reads the rows of the menu that is open on @p s.
 * @return the number of rows, or -1 when no menu box is on the screen.
 */
static int menu_read(PtySession *s, MenuRow rows[MENU_MAX_ROWS], int screen_rows) {
    int top = -1, n = 0;
    char t[400];
    for (int r = 0; r < screen_rows; r++) {
        pty_row_text(s, r, t, sizeof t);
        if (top < 0) {
            if (strstr(t, "Actions") && strstr(t, "\xe2\x94\x8c")) top = r;   /* ┌ Actions */
            continue;
        }
        if (strstr(t, "\xe2\x94\x94")) break;                                /* └ */
        const char *b = strstr(t, MENU_BORDER);
        if (!b) continue;
        const char *p = b + 3;
        while (*p == ' ' || *p == '>') p++;
        MenuRow *m = &rows[n];
        memset(m, 0, sizeof *m);
        if (*p && p[1] == ' ' && p[2] == ' ') {
            m->letter = *p;
            p += 3;
        }
        snprintf(m->text, sizeof m->text, "%s", p);
        /* cut at the right border */
        char *e = strstr(m->text, MENU_BORDER);
        if (e) *e = '\0';
        for (size_t i = strlen(m->text); i > 0 && m->text[i - 1] == ' '; i--) m->text[i - 1] = '\0';
        if (n + 1 < MENU_MAX_ROWS) n++;
    }
    return top < 0 ? -1 : n;
}

/** The index of the row whose text contains @p action, or -1. */
static int menu_find(const MenuRow *rows, int n, const char *action) {
    for (int i = 0; i < n; i++)
        if (strstr(rows[i].text, action)) return i;
    return -1;
}

/** Prints the rows read, for a failure message. */
static void menu_dump(const MenuRow *rows, int n) {
    for (int i = 0; i < n; i++) fprintf(stderr, "    menu row %d: [%c] %s\n", i, rows[i].letter ? rows[i].letter : '-', rows[i].text);
}

/** The quick key of the row whose text contains @p action, 0 when there is none. */
static char menu_letter(const MenuRow *rows, int n, const char *action) {
    int i = menu_find(rows, n, action);
    return i < 0 ? 0 : rows[i].letter;
}

/** 1 when every row has a letter and no letter is used twice. */
static int menu_letters_ok(const MenuRow *rows, int n) {
    for (int i = 0; i < n; i++) {
        if (!rows[i].letter) return 0;
        for (int j = i + 1; j < n; j++)
            if (rows[i].letter == rows[j].letter) return 0;
    }
    return n > 0;
}

/**
 * Opens the menu on @p s and checks that it has the @p nwant entries named in
 * @p want (each a part of the row's text), that every row shows a letter and
 * that the letters are unique.  Leaves the menu open.  Fills @p rows.
 */
#define ASSERT_MENU_LETTERS(s, screen_rows, rows, n, want, nwant, what)                    \
    do {                                                                                   \
        pty_send_str((s), ":");                                                            \
        ASSERT_WAIT_FOR((s), "Actions", WAIT_MS);                                          \
        pty_settle((s), 300);                                                              \
        (n) = menu_read((s), (rows), (screen_rows));                                       \
        if ((n) != (nwant) + 0 && (n) != -2) menu_dump((rows), (n));                       \
        ASSERT((n) >= (nwant), what ": the menu has the expected number of entries");      \
        ASSERT(menu_letters_ok((rows), (n)), what ": every entry has a unique letter");    \
        for (int mi_ = 0; mi_ < (nwant); mi_++)                                            \
            ASSERT(menu_find((rows), (n), (want)[mi_]) >= 0, what ": " "entry is listed"); \
    } while (0)

#endif /* TESTS_PTY_MENU_KEYS_H */
