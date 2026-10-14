/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-14 */
/* Unit tests of the autosaved compose draft: save, load, remove. */
#include "test_helpers.h"
#include "compose_draft.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void test_compose_draft(void) {
    char *old_home = getenv("HOME");
    char *old_xdg  = getenv("XDG_DATA_HOME");
    char *keep_home = old_home ? strdup(old_home) : NULL;
    char *keep_xdg  = old_xdg ? strdup(old_xdg) : NULL;
    setenv("HOME", TP("compose-draft-home"), 1);
    unsetenv("XDG_DATA_HOME");
    compose_draft_remove();

    ComposeDraft d;
    ASSERT(compose_draft_load(&d) != 0, "compose_draft: no draft before the first save");

    ComposeDraft w;
    memset(&w, 0, sizeof(w));
    snprintf(w.to, sizeof(w.to), "alice@example.org; bob@example.org");
    snprintf(w.cc, sizeof(w.cc), "carol@example.org");
    snprintf(w.subject, sizeof(w.subject), "Meeting: tomorrow");
    snprintf(w.in_reply_to, sizeof(w.in_reply_to), "<id@example.org>");
    snprintf(w.attach[0], sizeof(w.attach[0]), "/home/u/report.pdf");
    snprintf(w.attach[1], sizeof(w.attach[1]), "/home/u/notes.txt");
    w.attach_count = 2;
    w.body = "line one\n\nline three: \xe4\xbd\xa0\xe5\xa5\xbd\n";
    ASSERT(compose_draft_save(&w) == 0, "compose_draft: save succeeds");

    ASSERT(compose_draft_load(&d) == 0, "compose_draft: the saved draft loads");
    ASSERT(strcmp(d.to, w.to) == 0, "compose_draft: To round-trips");
    ASSERT(strcmp(d.cc, "carol@example.org") == 0, "compose_draft: Cc round-trips");
    ASSERT(d.bcc[0] == '\0', "compose_draft: an empty Bcc stays empty");
    ASSERT(strcmp(d.subject, "Meeting: tomorrow") == 0, "compose_draft: Subject round-trips");
    ASSERT(strcmp(d.in_reply_to, "<id@example.org>") == 0, "compose_draft: In-Reply-To round-trips");
    ASSERT(d.attach_count == 2 && strcmp(d.attach[1], "/home/u/notes.txt") == 0,
           "compose_draft: attachments round-trip in order");
    ASSERT(d.body && strcmp(d.body, w.body) == 0,
           "compose_draft: the body keeps blank lines and wide characters");
    compose_draft_free(&d);

    /* A body line that looks like a header stays in the body. */
    w.body = "To: not a header\nSubject: nor this\n";
    ASSERT(compose_draft_save(&w) == 0, "compose_draft: second save replaces the first");
    ASSERT(compose_draft_load(&d) == 0, "compose_draft: reload");
    ASSERT(strcmp(d.to, w.to) == 0, "compose_draft: header-like body lines do not touch the headers");
    ASSERT(d.body && strcmp(d.body, w.body) == 0, "compose_draft: header-like body lines are kept");
    compose_draft_free(&d);

    /* The file is private. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/.local/share/email-cli/compose-draft", TP("compose-draft-home"));
    struct stat st;
    ASSERT(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600, "compose_draft: the file is mode 0600");

    compose_draft_remove();
    ASSERT(compose_draft_load(&d) != 0, "compose_draft: removed draft is gone");
    compose_draft_remove();   /* a second removal is not an error */

    if (keep_home) setenv("HOME", keep_home, 1); else unsetenv("HOME");
    if (keep_xdg) setenv("XDG_DATA_HOME", keep_xdg, 1);
    free(keep_home);
    free(keep_xdg);
}
