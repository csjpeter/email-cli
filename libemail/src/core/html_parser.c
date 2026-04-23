#include "html_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Void elements (never have children) ───────────────────────────── */

static const char * const VOID_TAGS[] = {
    "area","base","br","col","embed","hr","img","input",
    "link","meta","param","source","track","wbr", NULL
};
static int is_void(const char *t) {
    if (!t) return 0;
    for (int i = 0; VOID_TAGS[i]; i++)
        if (strcmp(t, VOID_TAGS[i]) == 0) return 1;
    return 0;
}

/* Auto-close: opening one of these implicitly closes the previous sibling */
static const char * const AUTO_CLOSE_TAGS[] = {
    "li","p","dt","dd","tr","td","th", NULL
};
static int is_auto_close(const char *t) {
    if (!t) return 0;
    for (int i = 0; AUTO_CLOSE_TAGS[i]; i++)
        if (strcmp(t, AUTO_CLOSE_TAGS[i]) == 0) return 1;
    return 0;
}

/* ── Dynamic string ─────────────────────────────────────────────────── */

typedef struct { char *d; size_t n, cap; } Buf;

static int buf_push(Buf *b, char c) {
    if (b->n + 1 >= b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        char *t = realloc(b->d, nc);
        if (!t) return 0;
        b->d = t; b->cap = nc;
    }
    b->d[b->n++] = c;
    b->d[b->n]   = '\0';
    return 1;
}
static char *buf_take(Buf *b) {
    char *r = b->d; b->d = NULL; b->n = b->cap = 0; return r;
}
static void buf_free(Buf *b) { free(b->d); b->d = NULL; b->n = b->cap = 0; }

/* ── Node helpers ───────────────────────────────────────────────────── */

static HtmlNode *node_elem(char *tag) {
    HtmlNode *n = calloc(1, sizeof(*n));
    if (!n) { free(tag); return NULL; }
    n->type = HTML_NODE_ELEMENT; n->tag = tag; return n;
}
static HtmlNode *node_text(char *text) {
    HtmlNode *n = calloc(1, sizeof(*n));
    if (!n) { free(text); return NULL; }
    n->type = HTML_NODE_TEXT; n->text = text; return n;
}
static void attr_free_list(HtmlAttr *a) {
    while (a) { HtmlAttr *nx = a->next; free(a->name); free(a->value); free(a); a = nx; }
}
void html_node_free(HtmlNode *node) {
    if (!node) return;
    html_node_free(node->first_child);
    html_node_free(node->next_sibling);
    attr_free_list(node->attrs);
    free(node->tag); free(node->text); free(node);
}
const char *html_attr_get(const HtmlNode *node, const char *name) {
    if (!node || !name) return NULL;
    for (HtmlAttr *a = node->attrs; a; a = a->next)
        if (strcmp(a->name, name) == 0) return a->value;
    return NULL;
}
static void node_add_child(HtmlNode *parent, HtmlNode *child) {
    if (!parent || !child) return;
    child->parent = parent;
    if (!parent->first_child) { parent->first_child = child; return; }
    HtmlNode *last = parent->first_child;
    while (last->next_sibling) last = last->next_sibling;
    last->next_sibling = child;
}
static void attr_prepend(HtmlNode *elem, char *name, char *value) {
    HtmlAttr *a = calloc(1, sizeof(*a));
    if (!a) { free(name); free(value); return; }
    a->name = name; a->value = value; a->next = elem->attrs; elem->attrs = a;
}

/* ── Entity decoder ─────────────────────────────────────────────────── */

static const struct { const char *name; unsigned cp; } ENTITIES[] = {
    {"amp",'&'},{"lt",'<'},{"gt",'>'},{"quot",'"'},{"apos",'\''},
    {"nbsp",0xA0},{"copy",0xA9},{"reg",0xAE},{"mdash",0x2014},
    {"ndash",0x2013},{"hellip",0x2026},{"laquo",0xAB},{"raquo",0xBB},
    /* zero-width / formatting */
    {"zwnj",0x200C},{"zwj",0x200D},{"shy",0xAD},
    /* common symbols */
    {"bull",0x2022},{"middot",0xB7},{"trade",0x2122},{"euro",0x20AC},
    {"pound",0xA3},{"cent",0xA2},{"yen",0xA5},
    {"times",0xD7},{"divide",0xF7},
    /* Latin-1 supplement (HTML 4, U+00C0–U+00FF) */
    {"Agrave",0xC0},{"agrave",0xE0},{"Aacute",0xC1},{"aacute",0xE1},
    {"Acirc", 0xC2},{"acirc", 0xE2},{"Atilde",0xC3},{"atilde",0xE3},
    {"Auml",  0xC4},{"auml",  0xE4},{"Aring",  0xC5},{"aring",  0xE5},
    {"AElig", 0xC6},{"aelig", 0xE6},{"Ccedil",0xC7},{"ccedil",0xE7},
    {"Egrave",0xC8},{"egrave",0xE8},{"Eacute",0xC9},{"eacute",0xE9},
    {"Ecirc", 0xCA},{"ecirc", 0xEA},{"Euml",  0xCB},{"euml",  0xEB},
    {"Igrave",0xCC},{"igrave",0xEC},{"Iacute",0xCD},{"iacute",0xED},
    {"Icirc", 0xCE},{"icirc", 0xEE},{"Iuml",  0xCF},{"iuml",  0xEF},
    {"ETH",   0xD0},{"eth",   0xF0},{"Ntilde",0xD1},{"ntilde",0xF1},
    {"Ograve",0xD2},{"ograve",0xF2},{"Oacute",0xD3},{"oacute",0xF3},
    {"Ocirc", 0xD4},{"ocirc", 0xF4},{"Otilde",0xD5},{"otilde",0xF5},
    {"Ouml",  0xD6},{"ouml",  0xF6},{"Oslash",0xD8},{"oslash",0xF8},
    {"Ugrave",0xD9},{"ugrave",0xF9},{"Uacute",0xDA},{"uacute",0xFA},
    {"Ucirc", 0xDB},{"ucirc", 0xFB},{"Uuml",  0xDC},{"uuml",  0xFC},
    {"Yacute",0xDD},{"yacute",0xFD},{"THORN",  0xDE},{"thorn",  0xFE},
    {"szlig", 0xDF},{"yuml",  0xFF},{"iexcl",0xA1},{"iquest",0xBF},
    {"Iuml",  0xCF},{"ordf",  0xAA},{"ordm",  0xBA},
    {NULL,0}
};
static int cp_to_utf8(unsigned cp, char *out) {
    if (cp < 0x80)  { out[0]=(char)cp; return 1; }
    if (cp < 0x800) { out[0]=(char)(0xC0|(cp>>6)); out[1]=(char)(0x80|(cp&0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0]=(char)(0xE0|(cp>>12)); out[1]=(char)(0x80|((cp>>6)&0x3F));
        out[2]=(char)(0x80|(cp&0x3F)); return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0]=(char)(0xF0|(cp>>18)); out[1]=(char)(0x80|((cp>>12)&0x3F));
        out[2]=(char)(0x80|((cp>>6)&0x3F)); out[3]=(char)(0x80|(cp&0x3F)); return 4;
    }
    return 0;
}
static char *decode_entities(const char *in) {
    if (!in) return NULL;
    size_t len = strlen(in);
    char *out = malloc(len * 4 + 1);
    if (!out) return NULL;
    size_t n = 0;
    for (size_t i = 0; i < len; ) {
        if (in[i] != '&') { out[n++] = in[i++]; continue; }
        /* find ';' within reasonable range */
        size_t j = i + 1;
        while (j < len && in[j] != ';' && j - i < 14) j++;
        if (j < len && in[j] == ';' && j > i + 1) {
            const char *e = in + i + 1;
            size_t el = j - i - 1;
            unsigned cp = 0; int matched = 0;
            if (el > 0 && e[0] == '#') {
                if (el > 1 && (e[1]=='x'||e[1]=='X')) {
                    for (size_t k=2;k<el;k++) {
                        unsigned char c=(unsigned char)e[k];
                        if (c>='0'&&c<='9') cp=cp*16+(c-'0');
                        else if (c>='a'&&c<='f') cp=cp*16+(c-'a'+10);
                        else if (c>='A'&&c<='F') cp=cp*16+(c-'A'+10);
                    }
                } else {
                    for (size_t k=1;k<el;k++)
                        if (e[k]>='0'&&e[k]<='9') cp=cp*10+(unsigned char)(e[k]-'0');
                }
                matched = (cp > 0);
            } else if (el < 10) {
                char nm[10]={0}; memcpy(nm,e,el);
                for (int k=0; ENTITIES[k].name; k++)
                    if (strcmp(ENTITIES[k].name,nm)==0) { cp=ENTITIES[k].cp; matched=1; break; }
            }
            if (matched) {
                char utf8[5]={0}; int nb=cp_to_utf8(cp,utf8);
                for (int k=0;k<nb;k++) out[n++]=utf8[k];
                i = j + 1; continue;
            }
        }
        out[n++] = in[i++];
    }
    out[n] = '\0';
    return out;
}

/* ── Open-element stack ─────────────────────────────────────────────── */

#define STACK_MAX 256
typedef struct { HtmlNode *n[STACK_MAX]; int top; } Stack;
static void stk_push(Stack *s, HtmlNode *n) { if (s->top<STACK_MAX) s->n[s->top++]=n; }
static HtmlNode *stk_top(Stack *s) { return s->top>0 ? s->n[s->top-1] : NULL; }
static void stk_pop(Stack *s) { if (s->top>0) s->top--; }
/* Pop up to and including the nearest matching tag; return 1 if found */
static int stk_close(Stack *s, const char *tag) {
    for (int i=s->top-1; i>=0; i--)
        if (s->n[i]->tag && strcmp(s->n[i]->tag,tag)==0) { s->top=i; return 1; }
    return 0;
}

/* ── Parser ─────────────────────────────────────────────────────────── */

typedef enum {
    PS_TEXT,
    PS_LT,       /* saw '<' */
    PS_BANG,     /* saw '<!' */
    PS_CMT,      /* inside <!-- --> */
    PS_CMT_D1,   /* saw '-' inside comment */
    PS_CMT_DD,   /* saw '--' inside comment */
    PS_DECL,     /* inside <!DOCTYPE ...> */
    PS_OPEN,     /* reading open tag name */
    PS_CLOSE,    /* reading close tag name */
    PS_ATTR_SEP, /* between attributes */
    PS_ATTR_NM,  /* reading attribute name */
    PS_ATTR_EQ,  /* saw '=' */
    PS_ATTR_VQ,  /* in quoted attribute value */
    PS_ATTR_VU,  /* in unquoted attribute value */
} PState;

static void tolower_buf(char *s) {
    for (; s && *s; s++) *s = (char)tolower((unsigned char)*s);
}

/* Flush text buffer as a text node child of current stack top */
static void flush_text(Stack *stk, Buf *tb) {
    if (!tb->d || !tb->n) { buf_free(tb); return; }
    char *decoded = decode_entities(tb->d);
    HtmlNode *tn = node_text(decoded ? decoded : strdup(tb->d));
    if (!decoded) free(tb->d);
    buf_free(tb);
    if (tn) node_add_child(stk_top(stk), tn);
}

/* After completing a tag build: add to tree, push if non-void */
static void commit_elem(Stack *stk, HtmlNode *elem, int self_close) {
    if (!elem) return;
    if (is_auto_close(elem->tag)) {
        HtmlNode *top = stk_top(stk);
        if (top && top->tag && strcmp(top->tag, elem->tag) == 0)
            stk_pop(stk);
    }
    node_add_child(stk_top(stk), elem);
    if (!self_close && !is_void(elem->tag))
        stk_push(stk, elem);
}

HtmlNode *html_parse(const char *html) {
    if (!html) return NULL;

    HtmlNode *root = node_elem(strdup("__root__"));
    if (!root) return NULL;

    Stack stk; stk.top = 0;
    stk_push(&stk, root);

    PState state = PS_TEXT;
    Buf tb = {0}; /* text accumulator */
    Buf nb = {0}; /* tag/attr name */
    Buf vb = {0}; /* attr value */
    char qc = '"';

    HtmlNode *cur = NULL; /* element being built */

    const char *p = html;
    while (*p) {
        char c = *p;

        switch (state) {

        /* ── Text content ── */
        case PS_TEXT:
            if (c == '<') { flush_text(&stk, &tb); state = PS_LT; }
            else buf_push(&tb, c);
            break;

        /* ── Just saw '<' ── */
        case PS_LT:
            if (c == '/') { state = PS_CLOSE; }
            else if (c == '!') { state = PS_BANG; }
            else if (isalpha((unsigned char)c) || c == '_') {
                buf_push(&nb, c); state = PS_OPEN;
            } else {
                /* Not a real tag — treat '<' as text */
                buf_push(&tb, '<'); buf_push(&tb, c); state = PS_TEXT;
            }
            break;

        /* ── <!... ── */
        case PS_BANG:
            if (c == '-') {
                /* Peek: if next is also '-' it's a comment */
                if (p[1] == '-') { p++; state = PS_CMT; }
                else state = PS_DECL;
            } else state = PS_DECL;
            break;

        case PS_CMT:
            if (c == '-') state = PS_CMT_D1;
            break;
        case PS_CMT_D1:
            if (c == '-') state = PS_CMT_DD;
            else state = PS_CMT;
            break;
        case PS_CMT_DD:
            if (c == '>') state = PS_TEXT;
            else if (c != '-') state = PS_CMT;
            break;
        case PS_DECL:
            if (c == '>') state = PS_TEXT;
            break;

        /* ── Close tag name ── */
        case PS_CLOSE:
            if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == ':' || c == '.') {
                buf_push(&nb, c);
            } else if (c == '>') {
                if (nb.d) { tolower_buf(nb.d); stk_close(&stk, nb.d); }
                buf_free(&nb);
                state = PS_TEXT;
            }
            /* Skip whitespace and other chars inside </tag   > */
            break;

        /* ── Open tag name ── */
        case PS_OPEN: {
            int done = 0;
            if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == ':' || c == '.') {
                buf_push(&nb, c);
            } else {
                done = 1;
            }
            if (done && nb.d) {
                tolower_buf(nb.d);

                /* script / style: skip opening tag then body entirely */
                if (strcmp(nb.d, "script") == 0 || strcmp(nb.d, "style") == 0) {
                    char close_tag[16];
                    snprintf(close_tag, sizeof(close_tag), "</%s", nb.d);
                    buf_free(&nb);
                    /* Skip to end of opening tag */
                    while (*p && *p != '>') p++;
                    if (*p) p++; /* skip '>' */
                    /* Find closing tag (case-insensitive) */
                    const char *end = strcasestr(p, close_tag);
                    if (end) {
                        p = end + strlen(close_tag);
                        while (*p && *p != '>') p++;
                        /* p now points at '>' of closing tag */
                    } else {
                        p += strlen(p); /* skip to end */
                        if (*p) {} /* p already at '\0' */
                    }
                    state = PS_TEXT;
                    break;
                }

                char *tname = buf_take(&nb);
                cur = node_elem(tname);
                if (!cur) { state = PS_TEXT; break; }

                if (c == '>') {
                    commit_elem(&stk, cur, 0); cur = NULL; state = PS_TEXT;
                } else if (c == '/') {
                    commit_elem(&stk, cur, 1); cur = NULL;
                    while (*p && *p != '>') p++;
                    state = PS_TEXT;
                } else if (isspace((unsigned char)c)) {
                    state = PS_ATTR_SEP;
                } else {
                    /* Attribute name starting immediately */
                    buf_push(&nb, c); state = PS_ATTR_NM;
                }
            }
            break;
        }

        /* ── Between attributes ── */
        case PS_ATTR_SEP:
            if (c == '>') {
                commit_elem(&stk, cur, 0); cur = NULL; state = PS_TEXT;
            } else if (c == '/') {
                commit_elem(&stk, cur, 1); cur = NULL;
                while (*p && *p != '>') p++;
                state = PS_TEXT;
            } else if (!isspace((unsigned char)c)) {
                buf_push(&nb, c); state = PS_ATTR_NM;
            }
            break;

        /* ── Attribute name ── */
        case PS_ATTR_NM:
            if (c == '=') { state = PS_ATTR_EQ; }
            else if (c == '>') {
                /* Boolean attr */
                if (cur && nb.d && nb.n > 0)
                    attr_prepend(cur, buf_take(&nb), strdup(""));
                else buf_free(&nb);
                commit_elem(&stk, cur, 0); cur = NULL; state = PS_TEXT;
            } else if (isspace((unsigned char)c)) {
                if (cur && nb.d && nb.n > 0)
                    attr_prepend(cur, buf_take(&nb), strdup(""));
                else buf_free(&nb);
                state = PS_ATTR_SEP;
            } else {
                buf_push(&nb, c);
            }
            break;

        /* ── After '=' ── */
        case PS_ATTR_EQ:
            if (c == '"' || c == '\'') { qc = c; state = PS_ATTR_VQ; }
            else if (!isspace((unsigned char)c)) { buf_push(&vb, c); state = PS_ATTR_VU; }
            break;

        /* ── Quoted attribute value ── */
        case PS_ATTR_VQ:
            if (c == qc) {
                if (cur) attr_prepend(cur, buf_take(&nb), buf_take(&vb));
                else { buf_free(&nb); buf_free(&vb); }
                state = PS_ATTR_SEP;
            } else buf_push(&vb, c);
            break;

        /* ── Unquoted attribute value ── */
        case PS_ATTR_VU:
            if (isspace((unsigned char)c)) {
                if (cur) attr_prepend(cur, buf_take(&nb), buf_take(&vb));
                else { buf_free(&nb); buf_free(&vb); }
                state = PS_ATTR_SEP;
            } else if (c == '>') {
                if (cur) attr_prepend(cur, buf_take(&nb), buf_take(&vb));
                else { buf_free(&nb); buf_free(&vb); }
                commit_elem(&stk, cur, 0); cur = NULL; state = PS_TEXT;
            } else buf_push(&vb, c);
            break;

        } /* switch */

        if (*p) p++;
        else break;
    } /* while */

    flush_text(&stk, &tb);
    if (cur) html_node_free(cur); /* partially built element */
    buf_free(&nb); buf_free(&vb);
    return root;
}
