#include "path_complete.h"
#include "platform/terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/* ── Completion state ────────────────────────────────────────────────── */

static struct {
    char (*names)[256]; /* sorted match names (bare, no trailing '/') */
    int   count;
    int   idx;          /* currently highlighted entry */
    int   view_start;   /* first visible entry in the display row */
    char  dir[2048];    /* directory part (ends with '/') */
    char  expected[4096]; /* il->buf[0..cur] when this set was built */
    char  suffix[4096]; /* il->buf[cur..len] when this set was built */
} g_comp;

static void g_comp_free(void) {
    free(g_comp.names);
    memset(&g_comp, 0, sizeof(g_comp));
}

static int name_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* ── Rendering ───────────────────────────────────────────────────────── */

/* Render the completion bar one row below the input line.
 * Does nothing when there are no completions (preserves status bar). */
static void render_completions(const InputLine *il) {
    if (g_comp.count == 0) return;
    int row = il->trow + 1;
    printf("\033[%d;1H\033[2K", row);

    int tcols = terminal_cols();

    /* Keep view_start <= idx */
    if (g_comp.idx < g_comp.view_start)
        g_comp.view_start = g_comp.idx;

    /* Advance view_start until idx fits inside the visible window */
    for (;;) {
        int pos = 2 + (g_comp.view_start > 0 ? 2 : 0);
        int last = g_comp.view_start - 1;
        for (int i = g_comp.view_start; i < g_comp.count; i++) {
            int w    = (int)strlen(g_comp.names[i]) + 2;
            int need = (i < g_comp.count - 1) ? 3 : 0;
            if (pos + w + need > tcols) break;
            last = i;
            pos += w;
        }
        if (last >= g_comp.idx) break;
        g_comp.view_start++;
    }

    printf("\033[%d;1H  ", row);
    if (g_comp.view_start > 0)
        printf("\033[2m< \033[0m");

    int pos = 2 + (g_comp.view_start > 0 ? 2 : 0);
    for (int i = g_comp.view_start; i < g_comp.count; i++) {
        int w = (int)strlen(g_comp.names[i]) + 2;
        if (pos + w + (i < g_comp.count - 1 ? 3 : 0) > tcols) {
            printf("...");
            break;
        }
        if (i == g_comp.idx) printf("\033[7m");
        printf("%s", g_comp.names[i]);
        if (i == g_comp.idx) printf("\033[0m");
        printf("  ");
        pos += w;
    }
    fflush(stdout);
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Apply g_comp.names[g_comp.idx]: replace il->buf[0..cur] with dir+name,
 * then reattach the stored suffix. */
static void apply_comp(InputLine *il) {
    char head[4096];
    snprintf(head, sizeof(head), "%s%s", g_comp.dir, g_comp.names[g_comp.idx]);
    snprintf(il->buf, il->bufsz, "%s%s", head, g_comp.suffix);
    il->len = strlen(il->buf);
    il->cur = strlen(head);
    snprintf(g_comp.expected, sizeof(g_comp.expected), "%s", head);
}

/* Copy il->buf[0..il->cur] into head (NUL-terminated).
 * Returns 0 on success, -1 if head would overflow. */
static int make_head(const InputLine *il, char *head, size_t headsz) {
    if (il->cur >= headsz) return -1;
    memcpy(head, il->buf, il->cur);
    head[il->cur] = '\0';
    return 0;
}

/* ── Callbacks ───────────────────────────────────────────────────────── */

static void path_tab_fn(InputLine *il) {
    char head[4096];
    if (make_head(il, head, sizeof(head)) != 0) return;

    /* Cycling: head still matches last completion → advance */
    if (g_comp.count > 0 && strcmp(head, g_comp.expected) == 0) {
        g_comp.idx = (g_comp.idx + 1) % g_comp.count;
        apply_comp(il);
        return;
    }

    /* Fresh scan based on head */
    g_comp_free();
    snprintf(g_comp.suffix, sizeof(g_comp.suffix), "%s", il->buf + il->cur);

    const char *prefix;
    char *slash = strrchr(head, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - head + 1);
        if (dlen >= sizeof(g_comp.dir)) return;
        memcpy(g_comp.dir, head, dlen);
        g_comp.dir[dlen] = '\0';
        prefix = slash + 1;
    } else {
        strcpy(g_comp.dir, "./");
        prefix = head;
    }

    DIR *d = opendir(g_comp.dir);
    if (!d) return;

    int cap = 0;
    size_t pfxlen = strlen(prefix);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' && pfxlen == 0) continue;
        if (pfxlen > 0 && strncmp(ent->d_name, prefix, pfxlen) != 0) continue;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (g_comp.count == cap) {
            int nc = cap ? cap * 2 : 16;
            char (*tmp)[256] = realloc(g_comp.names,
                                       (size_t)nc * sizeof(*g_comp.names));
            if (!tmp) { closedir(d); g_comp_free(); return; }
            g_comp.names = tmp;
            cap = nc;
        }
        snprintf(g_comp.names[g_comp.count], 256, "%s", ent->d_name);
        g_comp.count++;
    }
    closedir(d);

    if (g_comp.count == 0) return;

    qsort(g_comp.names, (size_t)g_comp.count,
          sizeof(g_comp.names[0]), name_cmp);
    g_comp.idx        = 0;
    g_comp.view_start = 0;
    apply_comp(il);
}

static void path_shift_tab_fn(InputLine *il) {
    char head[4096];
    if (make_head(il, head, sizeof(head)) != 0) return;
    if (g_comp.count == 0 || strcmp(head, g_comp.expected) != 0) return;
    g_comp.idx = (g_comp.idx + g_comp.count - 1) % g_comp.count;
    apply_comp(il);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void path_complete_attach(InputLine *il) {
    il->tab_fn       = path_tab_fn;
    il->shift_tab_fn = path_shift_tab_fn;
    il->render_below = render_completions;
}

void path_complete_reset(void) {
    g_comp_free();
}
