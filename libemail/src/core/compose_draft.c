/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-10-14 */
/* The autosaved compose draft: one file in the data directory. */
#include "compose_draft.h"
#include "fs_util.h"
#include "../platform/path.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int draft_path(char *buf, size_t size, const char *suffix) {
    const char *base = platform_data_dir();
    if (!base) return -1;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/email-cli", base);
    if (fs_mkdir_p(dir, 0700) != 0) return -1;
    snprintf(buf, size, "%s/compose-draft%s", dir, suffix);
    return 0;
}

int compose_draft_save(const ComposeDraft *d) {
    char path[1100], tmp[1110];
    if (draft_path(path, sizeof(path), "") != 0 ||
        draft_path(tmp, sizeof(tmp), ".tmp") != 0) {
        fprintf(stderr, "email-cli: compose draft: no data directory\n");
        return -1;
    }
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "email-cli: compose draft: %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    fchmod(fileno(f), 0600);
    fprintf(f, "To: %s\nCc: %s\nBcc: %s\nSubject: %s\nIn-Reply-To: %s\n",
            d->to, d->cc, d->bcc, d->subject, d->in_reply_to);
    for (int i = 0; i < d->attach_count; i++)
        fprintf(f, "Attach: %s\n", d->attach[i]);
    fputc('\n', f);
    if (d->body) fputs(d->body, f);
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
        fprintf(stderr, "email-cli: compose draft: %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

#define COPY(dst, src) snprintf((dst), sizeof(dst), "%.*s", (int)sizeof(dst) - 1, (src))

int compose_draft_load(ComposeDraft *d) {
    memset(d, 0, sizeof(*d));
    char path[1100];
    if (draft_path(path, sizeof(path), "") != 0) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[4096];
    int in_body = 0;
    size_t blen = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!in_body) {
            size_t ll = strlen(line);
            while (ll > 0 && (line[ll - 1] == '\n' || line[ll - 1] == '\r')) line[--ll] = '\0';
            if (ll == 0) { in_body = 1; continue; }
            if      (strncmp(line, "To: ", 4) == 0)          COPY(d->to, line + 4);
            else if (strncmp(line, "Cc: ", 4) == 0)          COPY(d->cc, line + 4);
            else if (strncmp(line, "Bcc: ", 5) == 0)         COPY(d->bcc, line + 5);
            else if (strncmp(line, "Subject: ", 9) == 0)     COPY(d->subject, line + 9);
            else if (strncmp(line, "In-Reply-To: ", 13) == 0) COPY(d->in_reply_to, line + 13);
            else if (strncmp(line, "Attach: ", 8) == 0 && d->attach_count < 16)
                COPY(d->attach[d->attach_count++], line + 8);
        } else {
            size_t ll = strlen(line);
            char *nb = realloc(d->body, blen + ll + 1);
            if (!nb) { fclose(f); compose_draft_free(d); return -1; }
            d->body = nb;
            memcpy(d->body + blen, line, ll + 1);
            blen += ll;
        }
    }
    fclose(f);
    return 0;
}

void compose_draft_remove(void) {
    char path[1100];
    if (draft_path(path, sizeof(path), "") != 0) return;
    if (unlink(path) != 0 && errno != ENOENT)
        fprintf(stderr, "email-cli: compose draft: %s: %s\n", path, strerror(errno));
}

void compose_draft_free(ComposeDraft *d) {
    free(d->body);
    d->body = NULL;
}
