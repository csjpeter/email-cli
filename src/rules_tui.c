/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Peter Csaszar (Császár Péter) */
/* Created: 2026-11-21 */
/* The rules screens of email-tui: overview, rule form, dry run and undo. */
/**
 * @file rules_tui.c
 * @brief The rules overview and the rule form (EMAIL-312).
 *
 * Everything on screen is a libtuikit widget: the overview and the form are
 * tui_table views drawn through tui_frame_draw_table(); a condition is edited
 * in a tui_fieldset; the label pick is a tui_dialog_checklist; questions are
 * tui_dialog_confirm / prompt / menu; reports are a scrollable text view.
 * The condition of a rule is kept in two equal views: the list of clauses
 * (field, operator, value, joined with and/or) and the text of the `when`
 * expression, which may be edited as text; a text the clause list cannot hold
 * (parentheses that change the precedence, "all") is kept as text.
 */

#include "rules_tui.h"

#include "email_service.h"
#include "key_events.h"
#include "key_table.h"
#include "mail_rules.h"
#include "platform/terminal.h"
#include "raii.h"
#include "status_line.h"
#include "tui_frame.h"
#include "when_expr.h"

#include <tuikit/dialog.h>
#include <tuikit/fieldset.h>
#include <tuikit/table.h>
#include <tuikit/text.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Input and modals ────────────────────────────────────────────────── */

typedef struct { tui_decoder dec; KeyEvents winch; } RtIn;

static void in_begin(RtIn *in) {
    tui_decoder_init(&in->dec);
    key_events_begin(&in->winch);
}

static void in_end(RtIn *in) {
    key_events_end(&in->winch);
    tui_decoder_free(&in->dec);
}

/**
 * @brief The next key of a list view: q, Backspace, Left and h are Esc, g and G
 *        are Home and End, ctrl-c is Esc and asks the program to quit.
 * @return 1 a key, 0 a resize or another event (draw again), -1 the input ended.
 */
static int in_next(RtIn *in, tui_event *ev) {
    int r = key_events_next(STDIN_FILENO, &in->dec, ev, -1);
    if (r <= 0 || ev->type == TUI_EV_HANGUP) return -1;
    if (ev->type != TUI_EV_KEY) return 0;
    tui_frame_modal_key(ev, 1);
    if (ev->key == TUI_KEY_LEFT || TUI_EV_IS_CHAR(ev, 'h')) {
        ev->key = TUI_KEY_ESC; ev->mods = 0; ev->cp = 0; ev->utf8[0] = '\0';
    }
    return 1;
}

static int is_char(const tui_event *ev, int c) { return TUI_EV_IS_CHAR(ev, c); }

static int rt_confirm(const char *title, const char *text, int default_yes) {
    tui_dialog d;
    if (tui_dialog_confirm(&d, title, text, default_yes) != 0) return 0;
    int yes = tui_frame_run_dialog(&d) == TUI_DIALOG_OK && d.answer;
    tui_dialog_free(&d);
    return yes;
}

static void rt_message(const char *title, const char *text) {
    tui_dialog d;
    if (tui_dialog_message(&d, title, text) != 0) return;
    tui_frame_run_dialog(&d);
    tui_dialog_free(&d);
}

/** One line typed in a modal; 1 when confirmed. */
static int rt_prompt(const char *title, const char *text, char *buf, size_t size,
                     const char *initial) {
    tui_dialog d;
    char *init = strdup(initial ? initial : "");     /* @p initial may be @p buf itself */
    if (!init) return 0;
    int ok = 0;
    if (tui_dialog_prompt(&d, title, text, buf, size, init) == 0) {
        ok = tui_frame_run_dialog(&d) == TUI_DIALOG_OK;
        tui_dialog_free(&d);
    }
    free(init);
    return ok;
}

/** A menu; the chosen index, or -1. */
static int rt_menu(const char *title, const char *const *items, int n) {
    tui_dialog d;
    if (tui_dialog_menu(&d, title, items, n, 0) != 0) return -1;
    int pick = tui_frame_run_dialog(&d) == TUI_DIALOG_OK ? d.answer : -1;
    tui_dialog_free(&d);
    return pick;
}

static char *trim_dup(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    return strndup(s, n);
}

/** The status line of a rules view: hints, the help key, one message. */
static void bar(const tui_statusline_hint *h, int n, const char *msg, int cur, int total) {
    int rows = terminal_rows(), cols = terminal_cols();
    if (rows <= 0) rows = 24;
    if (cols <= 0) cols = 80;
    fflush(stdout);
    status_line_draw(rows, cols, h, n, "?", cur, total, msg);
}

/* ── The rule form ───────────────────────────────────────────────────── */

#define PREVIEW_SHOWN 3

typedef struct {
    char        name[256];
    char       *when;                 /* the condition text (heap, "" when none) */
    WhenClause *cl;                   /* the same condition as clauses, when it has that shape */
    int         ncl;
    int         custom;               /* 1: the text has no clause form (parentheses, "all") */
    char       *add[MAIL_RULE_MAX_LABELS]; int nadd;
    char       *rm[MAIL_RULE_MAX_LABELS];  int nrm;
    char        folder[256];
    char        fwd[256];
    int         disabled;
    int         action_index;
    int         changed;
    int         want_kind, want_idx;  /* after an edit: the row the cursor goes to (-1: stay) */
    /* the live preview */
    int         pv_state;             /* 0 no condition, 1 refused (pv_err), 2 counted */
    char        pv_err[256];
    int         pv_total, pv_count, pv_shown;
    RulePreviewMsg pv[PREVIEW_SHOWN];
} Draft;

static void draft_free(Draft *d) {
    free(d->when);
    when_clauses_free(d->cl, d->ncl);
    free(d->cl);
    for (int i = 0; i < d->nadd; i++) free(d->add[i]);
    for (int i = 0; i < d->nrm; i++) free(d->rm[i]);
    memset(d, 0, sizeof(*d));
}

/** Sets the condition text and re-derives the clause list from it. */
static void draft_set_when(Draft *d, const char *text) {
    free(d->when);
    d->when = strdup(text ? text : "");
    when_clauses_free(d->cl, d->ncl);
    free(d->cl);
    d->cl = NULL; d->ncl = 0; d->custom = 0;
    if (d->when[0] && when_clauses_parse(d->when, &d->cl, &d->ncl) != 0) {
        d->cl = NULL; d->ncl = 0;
        d->custom = 1;
    }
}

/** Rebuilds the condition text from the clause list. */
static void draft_rebuild_when(Draft *d) {
    free(d->when);
    d->when = when_clauses_build(d->cl, d->ncl);
    if (!d->when) d->when = strdup("");
}

static void draft_load(Draft *d, const MailRule *r) {
    memset(d, 0, sizeof(*d));
    d->want_kind = -1;
    d->action_index = 0;
    if (!r) {
        d->when = strdup("");
        return;
    }
    snprintf(d->name, sizeof(d->name), "%s", r->name ? r->name : "");
    draft_set_when(d, r->when ? r->when : "");
    for (int i = 0; i < r->then_add_count; i++) d->add[d->nadd++] = strdup(r->then_add_label[i]);
    for (int i = 0; i < r->then_rm_count; i++) d->rm[d->nrm++] = strdup(r->then_rm_label[i]);
    if (r->then_move_folder) snprintf(d->folder, sizeof(d->folder), "%s", r->then_move_folder);
    if (r->then_forward_to) snprintf(d->fwd, sizeof(d->fwd), "%s", r->then_forward_to);
    d->disabled = r->disabled;
    d->action_index = r->action_index;
}

static void draft_preview(Draft *d, const char *account) {
    d->pv_shown = 0;
    if (!d->when || !d->when[0]) { d->pv_state = 0; return; }
    char why[256];
    if (!when_check(d->when, why, sizeof(why))) {
        d->pv_state = 1;
        snprintf(d->pv_err, sizeof(d->pv_err), "%s", why);
        return;
    }
    if (email_service_rules_preview(account, d->when, &d->pv_total, &d->pv_count, d->pv,
                                    PREVIEW_SHOWN) != 0) {
        d->pv_state = 1;
        snprintf(d->pv_err, sizeof(d->pv_err), "the cached messages could not be read");
        return;
    }
    d->pv_state = 2;
    d->pv_shown = d->pv_count < PREVIEW_SHOWN ? d->pv_count : PREVIEW_SHOWN;
}

static const char *const FIELDS[] = { "from", "to", "subject", "label", "body", "age-gt", "age-lt" };
#define NFIELDS 7
static const char *const OPS[] = { "is", "contains", "matches glob" };

/** One clause in words: "from contains github". */
static void clause_text(const WhenClause *c, char *buf, size_t size) {
    const char *neg = c->negated ? "not " : "";
    if (strcmp(c->field, "age-gt") == 0)
        snprintf(buf, size, "%sage over %s days", neg, c->value);
    else if (strcmp(c->field, "age-lt") == 0)
        snprintf(buf, size, "%sage under %s days", neg, c->value);
    else
        snprintf(buf, size, "%s%s %s %s", neg, c->field, OPS[c->op], c->value);
}

typedef enum {
    FR_NAME, FR_ENABLED, FR_SECT, FR_CLAUSE, FR_ADD_COND, FR_TEXT, FR_CUSTOM,
    FR_ADD_LABEL, FR_RM_LABEL, FR_FOLDER, FR_FWD, FR_ADD_ACTION, FR_PREVIEW
} FormRowKind;

typedef struct { FormRowKind kind; int idx; char a[40]; char b[600]; } FormRow;

#define FORM_ROWS_MAX (4 * MAIL_RULE_MAX_LABELS + 64 + 128)

typedef struct { FormRow rows[FORM_ROWS_MAX]; int n; } FormRows;

static void row_add(FormRows *fr, FormRowKind kind, int idx, const char *a, const char *b) {
    if (fr->n >= FORM_ROWS_MAX) return;
    FormRow *r = &fr->rows[fr->n++];
    r->kind = kind;
    r->idx = idx;
    snprintf(r->a, sizeof(r->a), "%s", a ? a : "");
    snprintf(r->b, sizeof(r->b), "%s", b ? b : "");
}

static void form_rows(const Draft *d, FormRows *fr) {
    char buf[600];
    fr->n = 0;
    row_add(fr, FR_NAME, 0, "Name", d->name[0] ? d->name : "(not named yet)");
    row_add(fr, FR_ENABLED, 0, "Enabled", d->disabled ? "no  (the rule never fires)" : "yes");
    row_add(fr, FR_SECT, 0, "", "When the message matches");
    if (d->custom) {
        row_add(fr, FR_CUSTOM, 0, "", "(this condition has parentheses or 'all': edit it as text)");
    } else {
        for (int i = 0; i < d->ncl; i++) {
            clause_text(&d->cl[i], buf, sizeof(buf));
            row_add(fr, FR_CLAUSE, i, i == 0 ? "if" : d->cl[i].join_or ? "or" : "and", buf);
        }
        row_add(fr, FR_ADD_COND, 0, "", "+ add a condition");
    }
    row_add(fr, FR_TEXT, 0, "as text", d->when && d->when[0] ? d->when : "(none yet)");
    row_add(fr, FR_SECT, 0, "", "Then");
    for (int i = 0; i < d->nadd; i++) row_add(fr, FR_ADD_LABEL, i, "add label", d->add[i]);
    for (int i = 0; i < d->nrm; i++) row_add(fr, FR_RM_LABEL, i, "remove label", d->rm[i]);
    if (d->folder[0]) row_add(fr, FR_FOLDER, 0, "move to folder", d->folder);
    if (d->fwd[0]) row_add(fr, FR_FWD, 0, "forward to", d->fwd);
    row_add(fr, FR_ADD_ACTION, 0, "", "+ add an action");
    if (d->pv_state == 0) {
        row_add(fr, FR_PREVIEW, 0, "", "Preview: (no condition yet)");
    } else if (d->pv_state == 1) {
        snprintf(buf, sizeof(buf), "Preview: not available -- %s", d->pv_err);
        row_add(fr, FR_PREVIEW, 0, "", buf);
    } else {
        snprintf(buf, sizeof(buf), "Preview: %d of %d cached message%s match", d->pv_count,
                 d->pv_total, d->pv_total == 1 ? "" : "s");
        row_add(fr, FR_PREVIEW, 0, "", buf);
        for (int i = 0; i < d->pv_shown; i++) {
            snprintf(buf, sizeof(buf), "  %s -- %s", d->pv[i].from, d->pv[i].subject);
            row_add(fr, FR_PREVIEW, i + 1, "", buf);
        }
        if (d->pv_count > d->pv_shown) {
            snprintf(buf, sizeof(buf), "  ... and %d more", d->pv_count - d->pv_shown);
            row_add(fr, FR_PREVIEW, 99, "", buf);
        }
    }
}

static void form_row_cb(void *ctx, int i, tui_table_row *r) {
    const FormRows *fr = ctx;
    if (fr->rows[i].kind == FR_SECT || fr->rows[i].kind == FR_PREVIEW) {
        tui_table_row_kind(r, TUI_ROW_SECTION);
        tui_table_row_text(r, fr->rows[i].b);
    }
}

static void form_cell_cb(void *ctx, int i, int col, tui_table_cell *c) {
    const FormRows *fr = ctx;
    const FormRow *r = &fr->rows[i];
    if (col == 0) {
        tui_table_cell_put(c, r->a, tui_style_attrs(TUI_ATTR_BOLD));
    } else {
        tui_style st = TUI_STYLE_DEFAULT;
        if (r->kind == FR_ADD_COND || r->kind == FR_ADD_ACTION) st = TUI_STYLE_HINT;
        else if (r->kind == FR_CUSTOM) st = TUI_STYLE_HINT;
        tui_table_cell_put(c, r->b, st);
    }
}

/* ── Editing the parts of a draft ────────────────────────────────────── */

typedef struct { int first; } ClauseCheck;

static int clause_validate(void *user, const tui_fieldset *f, char *err, size_t errsz) {
    const ClauseCheck *cc = user;
    int base = cc->first ? 0 : 1;               /* the join field is not there for the first clause */
    const char *value = f->fields[base + 3].edit.buf;
    int field = f->fields[base].choice;
    int op = f->fields[base + 2].choice;
    char *v = trim_dup(value);
    int bad = -1;
    if (!v || !v[0]) {
        snprintf(err, errsz, "The value is empty.");
        bad = base + 3;
    } else if (field >= 5) {
        for (const char *p = v; *p; p++)
            if (*p < '0' || *p > '9') {
                snprintf(err, errsz, "An age is a number of days.");
                bad = base + 3;
                break;
            }
        if (bad < 0 && op != WOP_IS) {
            snprintf(err, errsz, "An age takes the operator 'is' only.");
            bad = base + 2;
        }
    } else if (strlen(v) > 1000) {
        snprintf(err, errsz, "The value is longer than 1000 characters; it would be cut.");
        bad = base + 3;
    }
    free(v);
    return bad;
}

/** Adds (index -1) or edits one clause in a fieldset modal; 1 when changed. */
static int draft_edit_clause(Draft *d, int index) {
    int first = index == 0 || (index < 0 && d->ncl == 0);
    static const char *const JOINS[] = { "and", "or" };
    static const char *const NOTS[] = { "no", "yes" };
    char valbuf[4096] = "";
    int cur_field = 0, cur_op = WOP_CONTAINS, cur_not = 0, cur_join = 0;
    if (index >= 0) {
        const WhenClause *c = &d->cl[index];
        for (int i = 0; i < NFIELDS; i++) if (strcmp(FIELDS[i], c->field) == 0) cur_field = i;
        cur_op = c->op; cur_not = c->negated; cur_join = c->join_or;
        snprintf(valbuf, sizeof(valbuf), "%s", c->value ? c->value : "");
    }
    tui_field fl[5];
    int n = 0;
    if (!first) tui_field_choice(&fl[n++], "Join:     ", JOINS, 2, cur_join);
    tui_field_choice(&fl[n++], "Field:    ", FIELDS, NFIELDS, cur_field);
    tui_field_choice(&fl[n++], "Not:      ", NOTS, 2, cur_not);
    tui_field_choice(&fl[n++], "Operator: ", OPS, 3, cur_op);
    tui_field_text(&fl[n++], "Value:    ", valbuf, sizeof(valbuf), valbuf);
    tui_fieldset fs;
    tui_fieldset_init(&fs, fl, n);
    ClauseCheck cc = { first };
    fs.validate = clause_validate;
    fs.user = &cc;
    tui_fieldset_focus(&fs, n - 1);
    int changed = 0;
    if (tui_frame_run_fieldset(index < 0 ? "Add a condition" : "Edit the condition", &fs) ==
        TUI_DIALOG_OK) {
        int base = first ? 0 : 1;
        WhenClause nc;
        memset(&nc, 0, sizeof(nc));
        nc.join_or = first ? 0 : fs.fields[0].choice;
        snprintf(nc.field, sizeof(nc.field), "%s", FIELDS[fs.fields[base].choice]);
        nc.negated = fs.fields[base + 1].choice;
        nc.op = fs.fields[base + 2].choice;
        nc.value = trim_dup(fs.fields[base + 3].edit.buf);
        if (index >= 0) {
            free(d->cl[index].value);
            d->cl[index] = nc;
        } else {
            WhenClause *t = realloc(d->cl, (size_t)(d->ncl + 1) * sizeof(WhenClause));
            if (t) {
                d->cl = t;
                d->cl[d->ncl++] = nc;
            } else {
                free(nc.value);
            }
        }
        draft_rebuild_when(d);
        changed = 1;
    }
    tui_fieldset_free(&fs);
    return changed;
}

static void clause_delete(Draft *d, int i) {
    free(d->cl[i].value);
    for (int k = i; k < d->ncl - 1; k++) d->cl[k] = d->cl[k + 1];
    d->ncl--;
    draft_rebuild_when(d);
}

/** Edits the condition as text; 1 when changed. */
static int draft_edit_text(Draft *d) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", d->when ? d->when : "");
    if (!rt_prompt("The condition as text",
                   "field:pattern joined by and / or, ! for not, (...) to group, 'all' for every message", buf,
                   sizeof(buf), buf))
        return 0;
    char *t = trim_dup(buf);
    char why[512];
    if (!t[0] || !when_check(t, why, sizeof(why))) {
        rt_message("Not a valid condition", t[0] ? why : "The condition is empty.");
        free(t);
        return 0;
    }
    int same = d->when && strcmp(d->when, t) == 0;
    if (!same) draft_set_when(d, t);
    free(t);
    return !same;
}

static void labels_set(char **arr, int *n, char **fresh, int nfresh) {
    for (int i = 0; i < *n; i++) free(arr[i]);
    *n = 0;
    for (int i = 0; i < nfresh && *n < MAIL_RULE_MAX_LABELS; i++) arr[(*n)++] = fresh[i];
}

/** Picks the labels of a rule's add or remove list from the labels the account knows. */
static int draft_pick_labels(Draft *d, const char *account, int adding) {
    char **arr = adding ? d->add : d->rm;
    int *n = adding ? &d->nadd : &d->nrm;
    int known_n = 0;
    char **known = email_service_rules_known_labels(account, &known_n);
    /* the choice list: the account's labels, the rule's own, the common ones, then "(new...)" */
    char *items[600];
    int ni = 0;
#define ADD_ITEM(s) do { int dup_ = 0; for (int q_ = 0; q_ < ni; q_++) if (strcmp(items[q_], (s)) == 0) dup_ = 1; \
                         if (!dup_ && ni < 560) items[ni++] = strdup(s); } while (0)
    for (int i = 0; i < *n; i++) ADD_ITEM(arr[i]);
    static const char *const COMMON[] = { "UNREAD", "_flagged", "_done", "_junk", "INBOX" };
    for (int i = 0; i < 5; i++) ADD_ITEM(COMMON[i]);
    for (int i = 0; i < known_n; i++) ADD_ITEM(known[i]);
    for (int i = 0; i < d->nadd; i++) ADD_ITEM(d->add[i]);
    for (int i = 0; i < d->nrm; i++) ADD_ITEM(d->rm[i]);
#undef ADD_ITEM
    int labels_n = ni;
    items[ni++] = strdup("(a new label...)");
    unsigned char ticked[600] = {0};
    for (int i = 0; i < labels_n; i++)
        for (int k = 0; k < *n; k++)
            if (strcmp(items[i], arr[k]) == 0) ticked[i] = 1;
    tui_dialog dlg;
    int changed = 0;
    if (tui_dialog_checklist(&dlg, adding ? "Labels to add" : "Labels to remove",
                             (const char *const *)items, ni, ticked, 0) == 0) {
        if (tui_frame_run_dialog(&dlg) == TUI_DIALOG_OK) {
            char *fresh[MAIL_RULE_MAX_LABELS];
            int nf = 0;
            for (int i = 0; i < labels_n && nf < MAIL_RULE_MAX_LABELS; i++)
                if (tui_dialog_is_checked(&dlg, i)) fresh[nf++] = strdup(items[i]);
            if (tui_dialog_is_checked(&dlg, ni - 1)) {
                char nb[512] = "";
                if (rt_prompt("New labels", "Label names, separated by commas", nb, sizeof(nb), "")) {
                    char *save = NULL;
                    for (char *t = strtok_r(nb, ",", &save); t && nf < MAIL_RULE_MAX_LABELS;
                         t = strtok_r(NULL, ",", &save)) {
                        char *l = trim_dup(t);
                        int dup = !l[0];
                        for (int k = 0; k < nf && !dup; k++) dup = strcmp(fresh[k], l) == 0;
                        if (dup) free(l); else fresh[nf++] = l;
                    }
                }
            }
            labels_set(arr, n, fresh, nf);
            changed = 1;
        }
        tui_dialog_free(&dlg);
    }
    for (int i = 0; i < ni; i++) free(items[i]);
    for (int i = 0; i < known_n; i++) free(known[i]);
    free(known);
    return changed;
}

static int draft_edit_line(char *dst, size_t size, const char *title, const char *text,
                           int need_at) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", dst);
    if (!rt_prompt(title, text, buf, sizeof(buf), buf)) return 0;
    char *t = trim_dup(buf);
    if (need_at && t[0] && !strchr(t, '@')) {
        rt_message("Not an address", "An address has an @ in it.");
        free(t);
        return 0;
    }
    int changed = strcmp(dst, t) != 0;
    snprintf(dst, size, "%s", t);
    free(t);
    return changed;
}

static int draft_add_action(Draft *d, const char *account) {
    static const char *const items[] = { "Add labels", "Remove labels", "Move to a folder",
                                         "Forward to an address" };
    switch (rt_menu("Add an action", items, 4)) {
    case 0: return draft_pick_labels(d, account, 1);
    case 1: return draft_pick_labels(d, account, 0);
    case 2: return draft_edit_line(d->folder, sizeof(d->folder), "Move to a folder",
                                   "The folder to move the message to (IMAP only); empty clears it", 0);
    case 3: return draft_edit_line(d->fwd, sizeof(d->fwd), "Forward to an address",
                                   "The address to forward to (stored; not yet executed); empty clears it", 1);
    default: return 0;
    }
}

/** Checks the draft and writes it to the account's rules; 1 saved. */
static int draft_save(Draft *d, const char *account, const char *orig_name, int orig_action) {
    char why[512];
    char *name = trim_dup(d->name);
    if (!name[0]) {
        rt_message("Cannot save", "The rule needs a name.");
        free(name);
        return 0;
    }
    if (!d->when || !d->when[0] || !when_check(d->when, why, sizeof(why))) {
        rt_message("Cannot save", d->when && d->when[0] ? why : "The rule needs a condition.");
        free(name);
        return 0;
    }
    MailRules *rules = mail_rules_load(account);
    if (!rules) rules = calloc(1, sizeof(MailRules));
    if (!rules) { free(name); return 0; }
    int at = orig_name ? mail_rules_find(rules, orig_name, orig_action) : -1;
    int clash = mail_rules_find(rules, name, d->action_index);
    if (clash >= 0 && clash != at) {
        rt_message("Cannot save", "Another rule has this name.");
        mail_rules_free(rules);
        free(name);
        return 0;
    }
    MailRule *mr;
    if (at >= 0) {
        mr = &rules->rules[at];
        mail_rule_release(mr);
    } else {
        mr = mail_rules_append(rules);
        if (!mr) { mail_rules_free(rules); free(name); return 0; }
    }
    mr->name = name;
    mr->action_index = d->action_index;
    mr->when = strdup(d->when);
    mr->disabled = d->disabled;
    for (int i = 0; i < d->nadd; i++) mr->then_add_label[mr->then_add_count++] = strdup(d->add[i]);
    for (int i = 0; i < d->nrm; i++) mr->then_rm_label[mr->then_rm_count++] = strdup(d->rm[i]);
    if (d->folder[0]) mr->then_move_folder = strdup(d->folder);
    if (d->fwd[0]) mr->then_forward_to = strdup(d->fwd);
    int rc = mail_rules_save(account, rules);
    mail_rules_free(rules);
    if (rc != 0) {
        rt_message("Cannot save", "The rules file could not be written.");
        return 0;
    }
    return 1;
}

static void form_help(void) {
    static const char *rows[][2] = {
        { "Enter",       "Edit the row under the cursor" },
        { "a",           "Add a condition (on a condition) or an action" },
        { "d",           "Delete the condition or action under the cursor" },
        { "Space",       "Switch the rule on or off (on the Enabled row)" },
        { "s",           "Save the rule" },
        { "?",           "Show this help" },
        { "Esc / q / BS", "Leave the form (asks when something changed)" },
    };
    email_service_help_popup(EKV_RULE, "Rule form shortcuts", rows, 7);
}

/**
 * @brief The rule form: a new rule (@p rule NULL) or an existing one.
 * @param saved_name  Receives the name of the saved rule (size 256).
 * @return 1 saved, 0 left without saving.
 */
static int rule_form(const char *account, const MailRule *rule, char *saved_name) {
    Draft d;
    draft_load(&d, rule);
    char orig_name[256] = "";
    int orig_action = rule ? rule->action_index : 0;
    if (rule && rule->name) snprintf(orig_name, sizeof(orig_name), "%s", rule->name);
    draft_preview(&d, account);

    static const tui_table_col cols[] = {
        { NULL, 16, 0, TUI_ALIGN_LEFT, 0, 0, 0 },
        { NULL, 20, 1, TUI_ALIGN_LEFT, 0, 0, 0 },
    };
    static FormRows fr;
    tui_table_model model = { form_row_cb, form_cell_cb, &fr };
    tui_table t;
    tui_table_init(&t, cols, 2, &model);
    t.flags |= TUI_TABLE_NO_HEADER;

    RtIn in;
    in_begin(&in);
    char msg[600] = "";
    int saved = 0, done = 0;
    while (!done) {
        form_rows(&d, &fr);
        tui_table_set_count(&t, fr.n);
        if (d.want_kind >= 0) {                 /* the cursor follows what the last edit made or left */
            for (int i = 0; i < fr.n; i++)
                if ((int)fr.rows[i].kind == d.want_kind && fr.rows[i].idx == d.want_idx) {
                    tui_table_select(&t, i);
                    break;
                }
            d.want_kind = -1;
        }
        int rows = terminal_rows(), cols_n = terminal_cols();
        if (rows <= 0) rows = 24;
        if (cols_n <= 0) cols_n = 80;
        char title[400];
        snprintf(title, sizeof(title), "%s for %s",
                 rule ? "Edit rule" : "New rule", account);
        tui_frame_draw_table(title, &t, rows, cols_n, 0, NULL, NULL);
        static const tui_statusline_hint h[] = {
            { "s", "save", SL_PRIO_HIGH },
            { "a", "add", SL_PRIO_HIGH },
            { "d", "delete", SL_PRIO_HIGH },
            { "Enter", "edit", SL_PRIO_MID },
            { "Esc", "cancel", SL_PRIO_KEEP },
        };
        bar(h, 5, msg[0] ? msg : NULL, 0, 0);
        msg[0] = '\0';

        tui_event ev;
        int r = in_next(&in, &ev);
        if (r < 0) break;
        if (r == 0) continue;
        int sel = tui_table_selected(&t);
        const FormRow *row = sel >= 0 && sel < fr.n ? &fr.rows[sel] : NULL;
        int refresh = 0;
        if (ev.key == TUI_KEY_ESC) {
            if (!d.changed || terminal_quit_requested() ||
                rt_confirm("Leave the form", "Throw away the changes of this rule?", 0))
                done = 1;
        } else if (is_char(&ev, '?')) {
            form_help();
        } else if (is_char(&ev, 's')) {
            if (draft_save(&d, account, rule ? orig_name : NULL, orig_action)) {
                snprintf(saved_name, 256, "%s", d.name);
                saved = 1;
                done = 1;
            }
        } else if (row && (ev.key == TUI_KEY_ENTER || is_char(&ev, 'e') || is_char(&ev, ' '))) {
            int ch = 0;
            switch (row->kind) {
            case FR_NAME: {
                char buf[256];
                snprintf(buf, sizeof(buf), "%s", d.name);
                if (rt_prompt("Rule name", "The name of the rule", buf, sizeof(buf), buf)) {
                    char *tn = trim_dup(buf);
                    if (strcmp(tn, d.name) != 0) { snprintf(d.name, sizeof(d.name), "%s", tn); ch = 1; }
                    free(tn);
                }
                break;
            }
            case FR_ENABLED: d.disabled = !d.disabled; ch = 1; break;
            case FR_CLAUSE:
                ch = draft_edit_clause(&d, row->idx); refresh = ch;
                d.want_kind = FR_CLAUSE; d.want_idx = row->idx;
                break;
            case FR_ADD_COND:
                ch = draft_edit_clause(&d, -1); refresh = ch;
                d.want_kind = ch ? FR_CLAUSE : FR_ADD_COND; d.want_idx = ch ? d.ncl - 1 : 0;
                break;
            case FR_TEXT: case FR_CUSTOM:
                ch = draft_edit_text(&d); refresh = ch;
                d.want_kind = d.custom ? FR_CUSTOM : FR_TEXT; d.want_idx = 0;
                break;
            case FR_ADD_LABEL: {
                char buf[256];
                snprintf(buf, sizeof(buf), "%s", d.add[row->idx]);
                if (rt_prompt("Label to add", "The label name", buf, sizeof(buf), buf)) {
                    char *tn = trim_dup(buf);
                    if (tn[0]) { free(d.add[row->idx]); d.add[row->idx] = tn; ch = 1; } else free(tn);
                }
                break;
            }
            case FR_RM_LABEL: {
                char buf[256];
                snprintf(buf, sizeof(buf), "%s", d.rm[row->idx]);
                if (rt_prompt("Label to remove", "The label name", buf, sizeof(buf), buf)) {
                    char *tn = trim_dup(buf);
                    if (tn[0]) { free(d.rm[row->idx]); d.rm[row->idx] = tn; ch = 1; } else free(tn);
                }
                break;
            }
            case FR_FOLDER:
                ch = draft_edit_line(d.folder, sizeof(d.folder), "Move to a folder",
                                     "The folder to move the message to (IMAP only); empty clears it", 0);
                break;
            case FR_FWD:
                ch = draft_edit_line(d.fwd, sizeof(d.fwd), "Forward to an address",
                                     "The address to forward to (stored; not yet executed); empty clears it", 1);
                break;
            case FR_ADD_ACTION:
                ch = draft_add_action(&d, account);
                d.want_kind = FR_ADD_ACTION; d.want_idx = 0;
                break;
            default: break;
            }
            if (ch) d.changed = 1;
        } else if (is_char(&ev, 'a') && row) {
            int ch;
            if (row->kind == FR_CLAUSE || row->kind == FR_ADD_COND || row->kind == FR_TEXT ||
                row->kind == FR_CUSTOM) {
                if (d.custom) {
                    rt_message("Add a condition", "This condition has parentheses or 'all'; edit it as text.");
                    ch = 0;
                } else {
                    ch = draft_edit_clause(&d, -1);
                    refresh = ch;
                    if (ch) { d.want_kind = FR_CLAUSE; d.want_idx = d.ncl - 1; }
                }
            } else {
                ch = draft_add_action(&d, account);
                d.want_kind = FR_ADD_ACTION; d.want_idx = 0;
            }
            if (ch) d.changed = 1;
        } else if (is_char(&ev, 'd') && row) {
            int ch = 1;
            switch (row->kind) {
            case FR_CLAUSE:
                clause_delete(&d, row->idx);
                refresh = 1;
                d.want_kind = d.ncl > 0 ? FR_CLAUSE : FR_ADD_COND;
                d.want_idx = row->idx < d.ncl ? row->idx : d.ncl - 1;
                break;
            case FR_ADD_LABEL:
                free(d.add[row->idx]);
                for (int k = row->idx; k < d.nadd - 1; k++) d.add[k] = d.add[k + 1];
                d.nadd--;
                d.want_kind = d.nadd > 0 ? FR_ADD_LABEL : FR_ADD_ACTION;
                d.want_idx = row->idx < d.nadd ? row->idx : d.nadd - 1;
                break;
            case FR_RM_LABEL:
                free(d.rm[row->idx]);
                for (int k = row->idx; k < d.nrm - 1; k++) d.rm[k] = d.rm[k + 1];
                d.nrm--;
                d.want_kind = d.nrm > 0 ? FR_RM_LABEL : FR_ADD_ACTION;
                d.want_idx = row->idx < d.nrm ? row->idx : d.nrm - 1;
                break;
            case FR_FOLDER: d.folder[0] = '\0'; d.want_kind = FR_ADD_ACTION; d.want_idx = 0; break;
            case FR_FWD: d.fwd[0] = '\0'; d.want_kind = FR_ADD_ACTION; d.want_idx = 0; break;
            default: ch = 0; break;
            }
            if (ch) d.changed = 1;
        } else {
            tui_table_handle(&t, &ev);
        }
        if (refresh) draft_preview(&d, account);
    }
    in_end(&in);
    draft_free(&d);
    return saved;
}

/* ── The overview ────────────────────────────────────────────────────── */

typedef struct {
    MailRules *rules;
    RuleStat  *stats;
    char     **sums;
    int        nsums;      /* entries of sums (the rules may change count after the load) */
} Overview;

static void ov_free(Overview *ov) {
    for (int i = 0; ov->sums && i < ov->nsums; i++) free(ov->sums[i]);
    free(ov->sums);
    free(ov->stats);
    mail_rules_free(ov->rules);
    memset(ov, 0, sizeof(*ov));
}

static void ov_load(Overview *ov, const char *account) {
    ov_free(ov);
    ov->rules = mail_rules_load(account);
    if (!ov->rules) ov->rules = calloc(1, sizeof(MailRules));
    int n = ov->rules ? ov->rules->count : 0;
    ov->stats = calloc((size_t)(n > 0 ? n : 1), sizeof(RuleStat));
    ov->sums = calloc((size_t)(n > 0 ? n : 1), sizeof(char *));
    if (n > 0 && ov->stats) email_service_rules_stats(account, ov->rules, ov->stats);
    ov->nsums = ov->sums ? n : 0;
    for (int i = 0; ov->sums && i < n; i++) ov->sums[i] = mail_rule_summary(&ov->rules->rules[i]);
}

static void ov_row_cb(void *ctx, int i, tui_table_row *r) {
    const Overview *ov = ctx;
    if (ov->rules->rules[i].disabled) tui_table_row_style(r, tui_style_attrs(TUI_ATTR_DIM));
}

static void ov_cell_cb(void *ctx, int i, int col, tui_table_cell *c) {
    const Overview *ov = ctx;
    const MailRule *r = &ov->rules->rules[i];
    char buf[64];
    switch (col) {
    case 0: snprintf(buf, sizeof(buf), "%d", i + 1); tui_table_cell_put(c, buf, TUI_STYLE_DEFAULT); break;
    case 1: tui_table_cell_put(c, r->name && r->name[0] ? r->name : "(unnamed)", TUI_STYLE_DEFAULT); break;
    case 2: tui_table_cell_put(c, ov->sums[i] ? ov->sums[i] : "", r->error ? TUI_STYLE_ERROR : TUI_STYLE_DEFAULT); break;
    case 3: tui_table_cell_put(c, r->disabled ? "off" : "on", r->disabled ? TUI_STYLE_HINT : TUI_STYLE_OK); break;
    case 4: snprintf(buf, sizeof(buf), "%d", ov->stats[i].matches); tui_table_cell_put(c, buf, TUI_STYLE_DEFAULT); break;
    default: tui_table_cell_put(c, ov->stats[i].last[0] ? ov->stats[i].last : "never", TUI_STYLE_DEFAULT); break;
    }
}

static void overview_help(void) {
    static const char *rows[][2] = {
        { "j / k",        "Next / previous rule (also the arrows, Home/End, PgUp/PgDn)" },
        { "Enter / e",    "Edit the rule in the rule form" },
        { "a",            "Add a rule" },
        { "d",            "Delete the rule (asks first)" },
        { "c",            "Copy the rule" },
        { "Space",        "Switch the rule on or off" },
        { "J / K",        "Move the rule down / up (rules run in this order)" },
        { "p",            "Preview what the rules would do (dry run; nothing changes)" },
        { "A",            "Apply the rules: preview, then ask, then change; undo is offered" },
        { "u",            "Undo the last apply" },
        { "?",            "Show this help" },
        { "Esc / q / BS", "Back to the message list" },
    };
    email_service_help_popup(EKV_RULES, "Rules shortcuts", rows, 12);
}

/** Reads a memstream result into a heap string. */
static char *capture_end(FILE *f, char **buf, size_t *len) {
    fclose(f);
    char *r = *buf ? *buf : strdup("");
    (void)len;
    return r;
}

/**
 * @brief Preview (dry run) of the whole set, then, when the owner confirms, the
 *        apply and the offer to undo it.
 */
static void apply_flow(const char *account, char *msg, size_t msgsz) {
    char *rep = NULL; size_t rl = 0;
    FILE *o = open_memstream(&rep, &rl);
    if (!o) return;
    int n = email_service_apply_rules_to(o, account, 1, 1);
    rep = capture_end(o, &rep, &rl);
    if (n < 0) {
        rt_message("Rules", "The rules could not be previewed.");
        free(rep);
        return;
    }
    char title[400];
    snprintf(title, sizeof(title), "  Rules preview (dry run, nothing changed): %s", account);
    int key = tui_frame_run_textview(title, rep, n > 0 ? "yA" : NULL, "apply");
    free(rep);
    if (!key) {
        snprintf(msg, msgsz, "Preview only: nothing was changed");
        return;
    }
    char q[200];
    snprintf(q, sizeof(q), "Change %d message%s now?  You can undo it right after.", n, n == 1 ? "" : "s");
    if (!rt_confirm("Apply the rules", q, 0)) {
        snprintf(msg, msgsz, "Not applied");
        return;
    }
    char *rep2 = NULL; size_t rl2 = 0;
    FILE *o2 = open_memstream(&rep2, &rl2);
    if (!o2) return;
    int done = email_service_apply_rules_to(o2, account, 0, 1);
    rep2 = capture_end(o2, &rep2, &rl2);
    snprintf(title, sizeof(title), "  Rules applied: %s (%d message%s changed)", account,
             done < 0 ? 0 : done, done == 1 ? "" : "s");
    tui_frame_run_textview(title, rep2, NULL, NULL);
    free(rep2);
    if (done > 0) {
        snprintf(q, sizeof(q), "%d message%s changed.  Undo this apply now?", done, done == 1 ? " was" : "s were");
        if (rt_confirm("Undo the apply", q, 0)) {
            char *rb = NULL; size_t rbl = 0;
            FILE *ro = open_memstream(&rb, &rbl);
            if (ro) {
                email_service_rules_revert_to(ro, ro, account);
                rb = capture_end(ro, &rb, &rbl);
                tui_frame_run_textview("  Undo of the last apply", rb, NULL, NULL);
                free(rb);
                snprintf(msg, msgsz, "Apply undone");
                return;
            }
        }
    }
    snprintf(msg, msgsz, "Applied: %d message%s changed", done < 0 ? 0 : done, done == 1 ? "" : "s");
}

static void undo_flow(const char *account, char *msg, size_t msgsz) {
    if (!rt_confirm("Undo the last apply", "Take back the last rules apply that was not yet undone?", 0)) {
        snprintf(msg, msgsz, "Nothing undone");
        return;
    }
    char *rb = NULL; size_t rbl = 0;
    FILE *ro = open_memstream(&rb, &rbl);
    if (!ro) return;
    int rc = email_service_rules_revert_to(ro, ro, account);
    rb = capture_end(ro, &rb, &rbl);
    tui_frame_run_textview(rc == 0 ? "  Undo of the last apply" : "  Undo of the last apply: not everything was restored",
                           rb, NULL, NULL);
    free(rb);
    snprintf(msg, msgsz, rc == 0 ? "Apply undone" : "Undo finished with refusals");
}

void rules_tui_editor(const Config *cfg) {
    if (!cfg || !cfg->user) return;
    const char *account = cfg->user;
    RAII_TERM_RAW TermRawState *_raw = terminal_raw_enter();

    static const tui_table_col cols[] = {
        { "#",           3, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
        { "Rule",        8, 1, TUI_ALIGN_LEFT,  0, 0, 0 },
        { "What it does", 20, 5, TUI_ALIGN_LEFT, 0, 0, 0 },
        { "On",          3, 0, TUI_ALIGN_LEFT,  0, 0, 0 },
        { "Matches",     7, 0, TUI_ALIGN_RIGHT, 0, 0, 0 },
        { "Last change", 16, 0, TUI_ALIGN_LEFT, 0, 0, 0 },
    };
    Overview ov = {0};
    tui_table_model model = { ov_row_cb, ov_cell_cb, &ov };
    tui_table t;
    tui_table_init(&t, cols, 6, &model);
    t.flags |= TUI_TABLE_RULER;

    RtIn in;
    in_begin(&in);
    char msg[600] = "";
    int dirty = 1, cursor = 0, done = 0;
    printf("\033[2J");
    while (!done) {
        if (dirty) {
            ov_load(&ov, account);
            dirty = 0;
        }
        int count = ov.rules ? ov.rules->count : 0;
        if (cursor >= count) cursor = count > 0 ? count - 1 : 0;
        tui_table_set_count(&t, count);
        tui_table_select(&t, cursor);
        if (count == 0)
            tui_table_set_message(&t, "(no rules -- press 'a' to add one)", TUI_STYLE_DEFAULT);
        else
            tui_table_set_message(&t, NULL, TUI_STYLE_DEFAULT);
        int rows = terminal_rows(), cols_n = terminal_cols();
        if (rows <= 0) rows = 24;
        if (cols_n <= 0) cols_n = 80;
        char title[512];
        snprintf(title, sizeof(title), "Rules for %s (%d rule%s)", account, count, count == 1 ? "" : "s");
        tui_frame_draw_table(title, &t, rows, cols_n, 0, NULL, NULL);
        static const tui_statusline_hint h[] = {
            { "u", "undo apply", SL_PRIO_LOW },
            { "A", "apply", SL_PRIO_MID },
            { "p", "preview", SL_PRIO_MID },
            { "J/K", "move", SL_PRIO_MID },
            { "Space", "on/off", SL_PRIO_MID },
            { "c", "copy", SL_PRIO_HIGH },
            { "d", "delete", SL_PRIO_HIGH },
            { "a", "add", SL_PRIO_HIGH },
        };
        bar(h, 8, msg[0] ? msg : NULL, count > 0 ? cursor + 1 : 0, count);
        msg[0] = '\0';

        tui_event ev;
        int r = in_next(&in, &ev);
        if (r < 0) break;
        if (r == 0) continue;
        if (ev.key == TUI_KEY_ESC) { done = 1; continue; }
        if (is_char(&ev, '?')) { overview_help(); continue; }
        if (is_char(&ev, 'a')) {
            char saved[256] = "";
            if (rule_form(account, NULL, saved)) {
                dirty = 1;
                snprintf(msg, sizeof(msg), "Rule added: %s", saved);
                ov_load(&ov, account);
                dirty = 0;
                for (int i = 0; ov.rules && i < ov.rules->count; i++)
                    if (ov.rules->rules[i].name && strcmp(ov.rules->rules[i].name, saved) == 0) cursor = i;
            }
            printf("\033[2J");
            continue;
        }
        if (is_char(&ev, 'p') || is_char(&ev, 'A')) {
            apply_flow(account, msg, sizeof(msg));
            dirty = 1;
            printf("\033[2J");
            continue;
        }
        if (is_char(&ev, 'u')) {
            undo_flow(account, msg, sizeof(msg));
            dirty = 1;
            printf("\033[2J");
            continue;
        }
        if (count == 0 || !ov.rules) { continue; }
        MailRule *rule = &ov.rules->rules[cursor];
        char nm[256];
        snprintf(nm, sizeof(nm), "%s", rule->name ? rule->name : "(unnamed)");
        if (ev.key == TUI_KEY_ENTER || is_char(&ev, 'e')) {
            char saved[256] = "";
            if (rule_form(account, rule, saved)) {
                snprintf(msg, sizeof(msg), "Rule saved: %s", saved);
                dirty = 1;
            }
            printf("\033[2J");
        } else if (is_char(&ev, 'd')) {
            char q[400];
            snprintf(q, sizeof(q), "Delete the rule \"%s\"?", nm);
            if (rt_confirm("Delete a rule", q, 0)) {
                mail_rules_remove_at(ov.rules, cursor);
                if (mail_rules_save(account, ov.rules) == 0)
                    snprintf(msg, sizeof(msg), "Rule deleted: %s", nm);
                else
                    snprintf(msg, sizeof(msg), "The rules could not be saved");
                dirty = 1;
            }
        } else if (is_char(&ev, 'c')) {
            MailRule *cp = mail_rules_append(ov.rules);
            if (cp) {
                rule = &ov.rules->rules[cursor];             /* the array may have moved */
                mail_rule_clone(rule, cp);
                free(cp->name);
                cp->name = NULL;                              /* copy_name looks at every name */
                cp->name = mail_rules_copy_name(ov.rules, nm);
                int at = ov.rules->count - 1;
                while (at > cursor + 1) { mail_rules_move(ov.rules, at, -1); at--; }
                if (mail_rules_save(account, ov.rules) == 0) {
                    snprintf(msg, sizeof(msg), "Copied as: %s", ov.rules->rules[cursor + 1].name);
                    cursor++;
                } else {
                    snprintf(msg, sizeof(msg), "The rules could not be saved");
                }
                dirty = 1;
            }
        } else if (is_char(&ev, ' ')) {
            rule->disabled = !rule->disabled;
            if (mail_rules_save(account, ov.rules) == 0)
                snprintf(msg, sizeof(msg), "Rule %s: %s", rule->disabled ? "switched off" : "switched on", nm);
            else
                snprintf(msg, sizeof(msg), "The rules could not be saved");
            dirty = 1;
        } else if (is_char(&ev, 'J') || is_char(&ev, 'K')) {
            int to = mail_rules_move(ov.rules, cursor, is_char(&ev, 'J') ? 1 : -1);
            if (to >= 0) {
                if (mail_rules_save(account, ov.rules) == 0) {
                    cursor = to;
                    snprintf(msg, sizeof(msg), "Rule moved to place %d: %s", to + 1, nm);
                } else {
                    snprintf(msg, sizeof(msg), "The rules could not be saved");
                }
                dirty = 1;
            }
        } else {
            tui_table_handle(&t, &ev);
            int s = tui_table_selected(&t);
            if (s >= 0) cursor = s;
        }
    }
    in_end(&in);
    ov_free(&ov);
    printf("\033[2J\033[H");
    fflush(stdout);
}
