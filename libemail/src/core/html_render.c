#include "html_render.h"
#include "html_parser.h"
#include "html_medium.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>

/* ── List stack ──────────────────────────────────────────────────────── */

#define LIST_MAX 16

typedef struct { int is_ol; int cnt; } ListFrame;

/* ── Render state ────────────────────────────────────────────────────── */

typedef struct {
    char     *buf;
    size_t    len, cap;
    int       col;         /* visible column (ANSI not counted) */
    int       width;       /* wrap width; 0 = no wrap */
    int       ansi;        /* emit ANSI escapes? */
    int       bold;        /* depth counter */
    int       italic;
    int       uline;
    int       color_fg;    /* depth: foreground color set by parse_style */
    int       color_bg;    /* depth: background color set by parse_style */
    int       skip;        /* depth: no output (script/style) */
    int       pre;         /* depth: no wrap */
    int       pending_nl;  /* buffered newlines to emit: 0, 1, or 2 */
    int       bq;          /* blockquote depth */
    ListFrame lists[LIST_MAX];
    int       list_top;
} RS;

/* ── Buffer ──────────────────────────────────────────────────────────── */

static void rs_push(RS *rs, char c) {
    if (rs->len + 2 > rs->cap) {
        size_t nc = rs->cap ? rs->cap * 2 : 512;
        char *t = realloc(rs->buf, nc);
        if (!t) return;
        rs->buf = t; rs->cap = nc;
    }
    rs->buf[rs->len++] = c;
    rs->buf[rs->len]   = '\0';
}
static void rs_str(RS *rs, const char *s) { for (; *s; s++) rs_push(rs, *s); }
static void rs_write(RS *rs, const char *s, int n)
    { for (int i = 0; i < n; i++) rs_push(rs, s[i]); }

/* ── UTF-8 helpers ───────────────────────────────────────────────────── */

static uint32_t utf8_adv(const char **p) {
    unsigned char c = (unsigned char)**p;
    uint32_t cp; int ex;
    if      (c < 0x80) { cp = c;        ex = 0; }
    else if (c < 0xC0) { (*p)++; return 0xFFFD; }
    else if (c < 0xE0) { cp = c & 0x1F; ex = 1; }
    else if (c < 0xF0) { cp = c & 0x0F; ex = 2; }
    else               { cp = c & 0x07; ex = 3; }
    (*p)++;
    for (int i = 0; i < ex; i++) {
        if ((**p & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (**p & 0x3F); (*p)++;
    }
    return cp;
}

static int str_vis_width(const char *s) {
    int w = 0;
    while (*s) {
        if ((unsigned char)*s == 0x1B && s[1] == '[') {
            s += 2; while (*s && *s != 'm') s++; if (*s) s++;
            continue;
        }
        w += html_medium_char_width(utf8_adv(&s));
    }
    return w;
}

/* ── Newline / prefix management ─────────────────────────────────────── */

static void emit_bq_prefix(RS *rs) {
    if (rs->col == 0 && rs->bq > 0) {
        for (int i = 0; i < rs->bq; i++) { rs_push(rs, '>'); rs_push(rs, ' '); }
        rs->col = rs->bq * 2;
    }
}

static void flush_nl(RS *rs) {
    if (!rs->pending_nl) return;
    /* Count consecutive newlines already at the end of the buffer so we
     * never accumulate more than pending_nl in a row. */
    int trailing = 0;
    for (int i = (int)rs->len - 1; i >= 0 && rs->buf[i] == '\n'; i--)
        trailing++;
    for (int i = trailing; i < rs->pending_nl; i++) rs_push(rs, '\n');
    rs->col = 0;
    rs->pending_nl = 0;
    emit_bq_prefix(rs);
}

static void req_nl(RS *rs, int n) {
    if (rs->pending_nl < n) rs->pending_nl = n;
}
static void block_open(RS *rs)  { if (rs->len > 0) req_nl(rs, 1); }
static void block_close(RS *rs) { req_nl(rs, 1); }
static void para_open(RS *rs)   { if (rs->len > 0) req_nl(rs, 2); }
static void para_close(RS *rs)  { req_nl(rs, 2); }

/* ── ANSI helpers ────────────────────────────────────────────────────── */

static void esc(RS *rs, const char *e) { if (rs->ansi) rs_str(rs, e); }
static void open_bold(RS *rs)    { if (rs->bold++   == 0) esc(rs, "\033[1m");  }
static void close_bold(RS *rs)   { if (--rs->bold   == 0) esc(rs, "\033[22m"); }
static void open_italic(RS *rs)  { if (rs->italic++ == 0) esc(rs, "\033[3m");  }
static void close_italic(RS *rs) { if (--rs->italic == 0) esc(rs, "\033[23m"); }
static void open_uline(RS *rs)   { if (rs->uline++  == 0) esc(rs, "\033[4m");  }
static void close_uline(RS *rs)  { if (--rs->uline  == 0) esc(rs, "\033[24m"); }

/* ── Inline CSS ──────────────────────────────────────────────────────── */

static int hex_val(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return 0;
}
static const struct { const char *name; int r,g,b; } CSS_COLORS[] = {
    {"black",0,0,0},{"silver",192,192,192},{"gray",128,128,128},
    {"white",255,255,255},{"maroon",128,0,0},{"red",255,0,0},
    {"purple",128,0,128},{"fuchsia",255,0,255},{"green",0,128,0},
    {"lime",0,255,0},{"olive",128,128,0},{"yellow",255,255,0},
    {"navy",0,0,128},{"blue",0,0,255},{"teal",0,128,128},{"aqua",0,255,255},
    {NULL,0,0,0}
};
static void apply_color(RS *rs, const char *v, int fg) {
    if (!rs->ansi) return;
    /* Background colors are never emitted: they break dark-theme terminals
     * and produce unreadable combinations when the email author's palette
     * does not match the user's terminal theme. */
    if (!fg) return;
    int r=-1,g=-1,b=-1;
    while (*v==' ') v++;
    if (*v=='#') {
        v++;
        size_t len=strlen(v); while(len>0&&v[len-1]==' ') len--;
        if (len==6) {
            r=hex_val(v[0])*16+hex_val(v[1]);
            g=hex_val(v[2])*16+hex_val(v[3]);
            b=hex_val(v[4])*16+hex_val(v[5]);
        } else if (len==3) {
            r=hex_val(v[0])*17; g=hex_val(v[1])*17; b=hex_val(v[2])*17;
        }
    } else {
        for (int i=0; CSS_COLORS[i].name; i++) {
            size_t nl=strlen(CSS_COLORS[i].name);
            if (strncasecmp(v, CSS_COLORS[i].name, nl)==0) {
                r=CSS_COLORS[i].r; g=CSS_COLORS[i].g; b=CSS_COLORS[i].b; break;
            }
        }
    }
    if (r<0) return;
    /* Suppress dark foreground colors (max component < 160): they are
     * unreadable on dark-theme terminals and common in newsletter HTML
     * (e.g. #333, #666, gray).  Only bright colours are emitted. */
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    if (mx < 160) return;
    char e[32]; snprintf(e,sizeof(e),"\033[38;2;%d;%d;%dm",r,g,b);
    rs_str(rs, e);
    rs->color_fg++;
}
static void parse_style(RS *rs, const char *style) {
    if (!style || !rs->ansi) return;
    const char *p = style;
    while (*p) {
        while (*p==' '||*p=='\t') p++;
        const char *ps=p; while(*p&&*p!=':') p++;
        if (!*p) break;
        size_t pl=(size_t)(p-ps); while(pl>0&&ps[pl-1]==' ') pl--;
        p++; while(*p==' ') p++;
        const char *vs=p; while(*p&&*p!=';') p++;
        size_t vl=(size_t)(p-vs); if(*p==';') p++;
        char prop[32]={0},val[64]={0};
        if(pl<sizeof(prop)) memcpy(prop,ps,pl);
        if(vl>0&&vl<sizeof(val)) memcpy(val,vs,vl);
        if      (!strcasecmp(prop,"font-weight")&&!strncasecmp(val,"bold",4))      { esc(rs,"\033[1m"); rs->bold++; }
        else if (!strcasecmp(prop,"font-style")&&!strncasecmp(val,"italic",6))     { esc(rs,"\033[3m"); rs->italic++; }
        else if (!strcasecmp(prop,"text-decoration")&&!strncasecmp(val,"underline",9)){ esc(rs,"\033[4m"); rs->uline++; }
        else if (!strcasecmp(prop,"color"))           apply_color(rs,val,1);
        else if (!strcasecmp(prop,"background-color")) apply_color(rs,val,0);
    }
}

/* ── Text emission with word wrap ────────────────────────────────────── */

static void emit_wrap_nl(RS *rs) {
    /* remove trailing space before the newline */
    if (rs->len > 0 && rs->buf[rs->len-1] == ' ') {
        rs->len--; rs->buf[rs->len] = '\0'; rs->col--;
    }
    rs_push(rs, '\n');
    rs->col = 0;
    emit_bq_prefix(rs);
}

static void emit_text(RS *rs, const char *text) {
    if (!text || !*text || rs->skip) return;
    flush_nl(rs);
    emit_bq_prefix(rs);

    if (rs->pre) {
        for (const char *p = text; *p; ) {
            if (*p == '\r') { p++; continue; }
            if (*p == '\n') {
                rs_push(rs, '\n'); rs->col = 0;
                emit_bq_prefix(rs);
                p++; continue;
            }
            const char *s = p;
            rs->col += html_medium_char_width(utf8_adv(&p));
            rs_write(rs, s, (int)(p - s));
        }
        return;
    }

    /* Normal mode: word-wrap */
    for (const char *p = text; *p; ) {
        if (isspace((unsigned char)*p)) {
            /* Collapse whitespace; only emit space if not at line start */
            int at_start = (rs->col <= rs->bq * 2);
            int already_space = (rs->len > 0 && rs->buf[rs->len-1] == ' ');
            if (!at_start && !already_space) {
                rs_push(rs, ' ');
                rs->col++;
            }
            while (*p && isspace((unsigned char)*p)) p++;
            continue;
        }

        /* Collect one word */
        const char *ws = p;
        int ww = 0;
        while (*p && !isspace((unsigned char)*p)) {
            const char *q = p;
            ww += html_medium_char_width(utf8_adv(&p));
            (void)q;
        }
        int wlen = (int)(p - ws);

        /* URL tokens (http://, https://, ftp://, mailto:) are always placed
         * on their own line so terminal URL-recognition works reliably.
         * They are never broken regardless of width. */
        int is_url = (wlen >= 6) && (
            strncmp(ws, "http://",  7) == 0 ||
            strncmp(ws, "https://", 8) == 0 ||
            strncmp(ws, "ftp://",   6) == 0 ||
            strncmp(ws, "mailto:",  7) == 0);

        if (is_url) {
            if (rs->col > rs->bq * 2) emit_wrap_nl(rs);  /* start own line */
            esc(rs, "\033[34m");        /* blue */
            rs_write(rs, ws, wlen);
            esc(rs, "\033[39m");        /* reset fg */
            rs->col += ww;
            rs_push(rs, '\n');          /* trailing newline: next content fresh line */
            rs->col = 0;
            emit_bq_prefix(rs);
            continue;
        }

        /* Wrap if needed (never wrap an otherwise-empty line) */
        if (rs->width > 0 && rs->col > rs->bq * 2 && rs->col + ww > rs->width)
            emit_wrap_nl(rs);

        rs_write(rs, ws, wlen);
        rs->col += ww;
    }
}

/* ── Whitespace helpers ──────────────────────────────────────────────── */

/** Returns 1 if s contains only ASCII whitespace, U+00A0 nbsp, U+200C zwnj,
 *  U+200D zwj, or U+00AD soft-hyphen — i.e. invisible/non-printing content. */
static int is_blank_str(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p <= ' ')                                    { p++;   continue; }
        if (p[0]==0xC2 && p[1]==0xA0)                    { p+=2;  continue; } /* nbsp  */
        if (p[0]==0xC2 && p[1]==0xAD)                    { p+=2;  continue; } /* shy   */
        if (p[0]==0xE2 && p[1]==0x80 && p[2]==0x8C)      { p+=3;  continue; } /* zwnj  */
        if (p[0]==0xE2 && p[1]==0x80 && p[2]==0x8D)      { p+=3;  continue; } /* zwj   */
        return 0;
    }
    return 1;
}

/** Collapses runs of >1 consecutive blank lines to exactly one blank line.
 *  Also trims trailing ASCII/nbsp/zwnj whitespace from each line.
 *  Preserves up to one trailing blank line.
 *  Takes ownership of s (frees it); returns a new heap string (or s on OOM). */
static char *compact_lines(char *s) {
    if (!s) return s;
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (!out) return s;

    const unsigned char *p = (const unsigned char *)s;
    char *q = out;
    int blank_pending = 0;  /* at most 1: whether a blank line is pending */
    int have_content  = 0;

    while (*p) {
        const unsigned char *ls = p;
        while (*p && *p != '\n') p++;
        int had_nl = (*p == '\n');
        const unsigned char *le = p;
        if (had_nl) p++;

        /* Trim trailing invisible chars (ASCII ws, nbsp C2A0, shy C2AD, zwnj/zwj E2808C/8D) */
        while (le > ls) {
            if (le[-1] <= ' ')                                              { le--;   continue; }
            if (le>=ls+2 && le[-2]==0xC2 && (le[-1]==0xA0||le[-1]==0xAD)) { le-=2;  continue; }
            if (le>=ls+3 && le[-3]==0xE2 && le[-2]==0x80 &&
                (le[-1]==0x8C||le[-1]==0x8D))                              { le-=3;  continue; }
            break;
        }

        if (le == ls) {  /* blank line */
            if (had_nl) blank_pending = 1;
        } else {         /* non-blank line */
            if (blank_pending && have_content) *q++ = '\n';
            blank_pending = 0;
            have_content = 1;
            memcpy(q, ls, (size_t)(le - ls));
            q += (size_t)(le - ls);
            if (had_nl) *q++ = '\n';
        }
    }
    /* Preserve at most one trailing blank line */
    if (blank_pending) *q++ = '\n';

    *q = '\0';
    free(s);
    return out;
}

/* ── Tag open / close ────────────────────────────────────────────────── */

static void traverse(RS *rs, const HtmlNode *node);

static void tag_open(RS *rs, const HtmlNode *node) {
    if (!node->tag) return;
    const char *t = node->tag;

    /* Inline styles (applied before tag-specific behavior) */
    const char *style = html_attr_get(node, "style");
    if (style) parse_style(rs, style);

    if      (!strcmp(t,"b")||!strcmp(t,"strong"))  open_bold(rs);
    else if (!strcmp(t,"i")||!strcmp(t,"em"))       open_italic(rs);
    else if (!strcmp(t,"u"))                        open_uline(rs);
    else if (!strcmp(t,"s")||!strcmp(t,"del")||!strcmp(t,"strike")) esc(rs,"\033[9m");
    else if (!strcmp(t,"br"))                       req_nl(rs, 1);
    else if (!strcmp(t,"hr")) {
        block_open(rs); flush_nl(rs); emit_bq_prefix(rs);
        int w = rs->width > 0 ? rs->width - rs->col : 20;
        for (int i = 0; i < w; i++) rs_push(rs, '-');
        rs->col += w;
        block_close(rs);
    }
    else if (!strcmp(t,"p"))                        para_open(rs);
    else if (!strcmp(t,"div")||!strcmp(t,"article")||!strcmp(t,"section")||
             !strcmp(t,"main")||!strcmp(t,"header")||!strcmp(t,"footer")||
             !strcmp(t,"nav")||!strcmp(t,"aside"))  block_open(rs);
    else if (t[0]=='h' && t[1]>='1' && t[1]<='6' && !t[2]) {
        para_open(rs); open_bold(rs);
    }
    else if (!strcmp(t,"ul")) {
        if (rs->list_top < LIST_MAX) rs->lists[rs->list_top++] = (ListFrame){0, 0};
        block_open(rs);
    }
    else if (!strcmp(t,"ol")) {
        if (rs->list_top < LIST_MAX) rs->lists[rs->list_top++] = (ListFrame){1, 0};
        block_open(rs);
    }
    else if (!strcmp(t,"li")) {
        block_open(rs);
        flush_nl(rs);
        emit_bq_prefix(rs);
        if (rs->list_top > 0 && rs->lists[rs->list_top-1].is_ol) {
            rs->lists[rs->list_top-1].cnt++;
            char buf[16];
            int n = snprintf(buf, sizeof(buf), "%d. ", rs->lists[rs->list_top-1].cnt);
            rs_str(rs, buf);
            rs->col += n;
        } else {
            /* U+2022 BULLET: UTF-8 E2 80 A2 */
            rs_push(rs,(char)0xE2); rs_push(rs,(char)0x80); rs_push(rs,(char)0xA2);
            rs_push(rs, ' ');
            rs->col += 2; /* bullet=1 col, space=1 col */
        }
    }
    else if (!strcmp(t,"blockquote")) {
        block_open(rs);
        rs->bq++;
        /* if something was already on the line, start fresh */
        if (rs->col > 0) { flush_nl(rs); } else emit_bq_prefix(rs);
    }
    else if (!strcmp(t,"pre"))    { block_open(rs); rs->pre++; }
    else if (!strcmp(t,"code"))   { /* inline: no special formatting */ }
    else if (!strcmp(t,"img")) {
        const char *alt = html_attr_get(node, "alt");
        if (alt && *alt && !is_blank_str(alt)) {
            flush_nl(rs); emit_bq_prefix(rs);
            rs_push(rs, '[');
            rs_str(rs, alt);
            rs_push(rs, ']');
            rs->col += 2 + str_vis_width(alt);
        }
    }
    else if (!strcmp(t,"a")) {
        const char *href = html_attr_get(node, "href");
        if (href && *href && href[0] != '#' && strncmp(href, "javascript:", 11) != 0)
            if (rs->ansi) { esc(rs, "\033[34m"); rs->color_fg++; }
    }
    else if (!strcmp(t,"td")||!strcmp(t,"th")) {
        if (rs->col > rs->bq * 2) { rs_push(rs, '\t'); rs->col++; }
    }
    else if (!strcmp(t,"tr")||!strcmp(t,"table")) block_open(rs);
    else if (!strcmp(t,"script")||!strcmp(t,"style")) rs->skip++;
    else if (!strcmp(t,"textarea")||!strcmp(t,"button")) block_open(rs);
    else if (!strcmp(t,"input")) {
        const char *val = html_attr_get(node, "value");
        if (val && *val) {
            flush_nl(rs); emit_bq_prefix(rs);
            rs_str(rs, val);
            rs->col += str_vis_width(val);
        }
    }
    /* __root__ and unknown tags: traverse children unchanged */
}

static void tag_close(RS *rs, const HtmlNode *node) {
    if (!node->tag) return;
    const char *t = node->tag;

    if      (!strcmp(t,"b")||!strcmp(t,"strong"))  close_bold(rs);
    else if (!strcmp(t,"i")||!strcmp(t,"em"))       close_italic(rs);
    else if (!strcmp(t,"u"))                        close_uline(rs);
    else if (!strcmp(t,"s")||!strcmp(t,"del")||!strcmp(t,"strike")) esc(rs,"\033[29m");
    else if (!strcmp(t,"p"))                        para_close(rs);
    else if (!strcmp(t,"div")||!strcmp(t,"article")||!strcmp(t,"section")||
             !strcmp(t,"main")||!strcmp(t,"header")||!strcmp(t,"footer")||
             !strcmp(t,"nav")||!strcmp(t,"aside"))  block_close(rs);
    else if (t[0]=='h' && t[1]>='1' && t[1]<='6' && !t[2]) {
        close_bold(rs); para_close(rs);
    }
    else if (!strcmp(t,"ul")||!strcmp(t,"ol")) {
        if (rs->list_top > 0) rs->list_top--;
        block_close(rs);
    }
    else if (!strcmp(t,"li"))  req_nl(rs, 1);
    else if (!strcmp(t,"blockquote")) {
        if (rs->bq > 0) rs->bq--;
        block_close(rs);
    }
    else if (!strcmp(t,"pre"))  { if (rs->pre>0) rs->pre--; block_close(rs); }
    else if (!strcmp(t,"a")) {
        const char *href = html_attr_get(node, "href");
        if (href && *href && href[0] != '#' &&
            strncmp(href, "javascript:", 11) != 0) {
            if (rs->ansi && rs->color_fg > 0) { esc(rs, "\033[39m"); rs->color_fg--; }
            esc(rs, "\033[34m");
            emit_text(rs, href);
            esc(rs, "\033[39m");
        }
    }
    else if (!strcmp(t,"script")||!strcmp(t,"style")) { if (rs->skip>0) rs->skip--; }
    else if (!strcmp(t,"tr"))   req_nl(rs, 1);
    else if (!strcmp(t,"table")) block_close(rs);
    else if (!strcmp(t,"textarea")||!strcmp(t,"button")) block_close(rs);
}

/* ── Tree traversal ──────────────────────────────────────────────────── */

static void traverse(RS *rs, const HtmlNode *node) {
    if (!node) return;
    if (node->type == HTML_NODE_TEXT) {
        emit_text(rs, node->text);
        return;
    }
    /* Snapshot style depth so parse_style side-effects from inline
     * style= attributes are balanced even when tag_close has no handler
     * for this tag (e.g. <a>, <span>, <td> with style="…"). */
    int bold_sv     = rs->bold;
    int italic_sv   = rs->italic;
    int uline_sv    = rs->uline;
    int color_fg_sv = rs->color_fg;
    int color_bg_sv = rs->color_bg;
    tag_open(rs, node);
    for (const HtmlNode *c = node->first_child; c; c = c->next_sibling)
        traverse(rs, c);
    tag_close(rs, node);
    /* Close any depth-tracked style that tag_close left open */
    while (rs->uline    > uline_sv)    close_uline(rs);
    while (rs->italic   > italic_sv)   close_italic(rs);
    while (rs->bold     > bold_sv)     close_bold(rs);
    if (rs->ansi) {
        if (rs->color_fg > color_fg_sv) { esc(rs, "\033[39m"); rs->color_fg = color_fg_sv; }
        if (rs->color_bg > color_bg_sv) { esc(rs, "\033[49m"); rs->color_bg = color_bg_sv; }
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

char *html_render(const char *html, int width, int ansi) {
    if (!html) return NULL;

    HtmlNode *root = html_parse(html);
    if (!root) {
        char *empty = malloc(1);
        if (empty) *empty = '\0';
        return empty;
    }

    RS rs;
    memset(&rs, 0, sizeof(rs));
    rs.width = width;
    rs.ansi  = ansi;

    /* Traverse root's children (root itself is synthetic __root__) */
    for (const HtmlNode *c = root->first_child; c; c = c->next_sibling)
        traverse(&rs, c);

    /* Flush trailing newlines */
    flush_nl(&rs);

    html_node_free(root);

    if (!rs.buf) {
        char *empty = malloc(1);
        if (empty) *empty = '\0';
        return empty;
    }
    return compact_lines(rs.buf);
}
