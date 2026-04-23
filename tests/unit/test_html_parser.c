#include "test_helpers.h"
#include "html_parser.h"
#include "raii.h"
#include <string.h>
#include <stdlib.h>

void test_html_parser(void) {

    /* 1. NULL input → NULL */
    {
        HtmlNode *r = html_parse(NULL);
        ASSERT(r == NULL, "html_parse(NULL) should return NULL");
    }

    /* 2. Empty string → root with no children */
    {
        HtmlNode *r = html_parse("");
        ASSERT(r != NULL, "html_parse empty: root not NULL");
        ASSERT(r->first_child == NULL, "html_parse empty: no children");
        html_node_free(r);
    }

    /* 3. Plain text → single TEXT child */
    {
        HtmlNode *r = html_parse("Hello World");
        ASSERT(r != NULL, "plain text: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c != NULL, "plain text: has child");
        ASSERT(c->type == HTML_NODE_TEXT, "plain text: child is TEXT");
        ASSERT(strcmp(c->text, "Hello World") == 0, "plain text: content matches");
        html_node_free(r);
    }

    /* 4. <b>text</b> → ELEMENT(b) → TEXT child */
    {
        HtmlNode *r = html_parse("<b>text</b>");
        ASSERT(r != NULL, "b: root not NULL");
        HtmlNode *b = r->first_child;
        ASSERT(b != NULL, "b: element present");
        ASSERT(b->type == HTML_NODE_ELEMENT, "b: type ELEMENT");
        ASSERT(strcmp(b->tag, "b") == 0, "b: tag is 'b'");
        HtmlNode *txt = b->first_child;
        ASSERT(txt != NULL, "b: has text child");
        ASSERT(txt->type == HTML_NODE_TEXT, "b: child is TEXT");
        ASSERT(strcmp(txt->text, "text") == 0, "b: text content matches");
        html_node_free(r);
    }

    /* 5. Void element <br> — no children */
    {
        HtmlNode *r = html_parse("<br>");
        ASSERT(r != NULL, "br: root not NULL");
        HtmlNode *br = r->first_child;
        ASSERT(br != NULL, "br: element present");
        ASSERT(strcmp(br->tag, "br") == 0, "br: tag is 'br'");
        ASSERT(br->first_child == NULL, "br: no children");
        html_node_free(r);
    }

    /* 6. Self-closing <br/> — no children */
    {
        HtmlNode *r = html_parse("<br/>");
        ASSERT(r != NULL, "br/: root not NULL");
        HtmlNode *br = r->first_child;
        ASSERT(br != NULL, "br/: element present");
        ASSERT(br->first_child == NULL, "br/: no children");
        html_node_free(r);
    }

    /* 7. Attributes: double-quoted */
    {
        HtmlNode *r = html_parse("<img src=\"foo.png\" alt=\"bar\">");
        ASSERT(r != NULL, "attrs: root not NULL");
        HtmlNode *img = r->first_child;
        ASSERT(img != NULL, "attrs: img present");
        const char *src = html_attr_get(img, "src");
        const char *alt = html_attr_get(img, "alt");
        ASSERT(src && strcmp(src, "foo.png") == 0, "attrs: src matches");
        ASSERT(alt && strcmp(alt, "bar") == 0, "attrs: alt matches");
        html_node_free(r);
    }

    /* 8. Attributes: unquoted */
    {
        HtmlNode *r = html_parse("<input type=text>");
        ASSERT(r != NULL, "unquoted attr: root not NULL");
        HtmlNode *inp = r->first_child;
        ASSERT(inp != NULL, "unquoted attr: input present");
        const char *type = html_attr_get(inp, "type");
        ASSERT(type && strcmp(type, "text") == 0, "unquoted attr: type matches");
        html_node_free(r);
    }

    /* 9. Boolean attribute */
    {
        HtmlNode *r = html_parse("<input disabled>");
        ASSERT(r != NULL, "bool attr: root not NULL");
        HtmlNode *inp = r->first_child;
        ASSERT(inp != NULL, "bool attr: input present");
        const char *dis = html_attr_get(inp, "disabled");
        ASSERT(dis != NULL, "bool attr: disabled present");
        html_node_free(r);
    }

    /* 10. Entity: &amp; → & */
    {
        HtmlNode *r = html_parse("a &amp; b");
        ASSERT(r != NULL, "entity amp: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "entity amp: text node");
        ASSERT(strcmp(c->text, "a & b") == 0, "entity amp: decoded");
        html_node_free(r);
    }

    /* 11. Entity: &lt; → < */
    {
        HtmlNode *r = html_parse("&lt;tag&gt;");
        ASSERT(r != NULL, "entity lt gt: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "entity lt gt: text node");
        ASSERT(strcmp(c->text, "<tag>") == 0, "entity lt gt: decoded");
        html_node_free(r);
    }

    /* 12. Entity: &nbsp; → UTF-8 0xC2 0xA0 */
    {
        HtmlNode *r = html_parse("&nbsp;");
        ASSERT(r != NULL, "entity nbsp: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "entity nbsp: text node");
        ASSERT(c->text && (unsigned char)c->text[0] == 0xC2
               && (unsigned char)c->text[1] == 0xA0, "entity nbsp: UTF-8 correct");
        html_node_free(r);
    }

    /* 13. Numeric entity: &#65; → 'A' */
    {
        HtmlNode *r = html_parse("&#65;");
        ASSERT(r != NULL, "entity #65: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "entity #65: text node");
        ASSERT(c->text && c->text[0] == 'A', "entity #65: decoded to A");
        html_node_free(r);
    }

    /* 14. Hex entity: &#x41; → 'A' */
    {
        HtmlNode *r = html_parse("&#x41;");
        ASSERT(r != NULL, "entity #x41: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "entity #x41: text node");
        ASSERT(c->text && c->text[0] == 'A', "entity #x41: decoded to A");
        html_node_free(r);
    }

    /* 15. Comment ignored */
    {
        HtmlNode *r = html_parse("<!-- foo -->bar");
        ASSERT(r != NULL, "comment: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c != NULL, "comment: has child");
        ASSERT(c->type == HTML_NODE_TEXT, "comment: child is TEXT");
        ASSERT(strcmp(c->text, "bar") == 0, "comment: only text after comment");
        html_node_free(r);
    }

    /* 16. <script> body not in tree */
    {
        HtmlNode *r = html_parse("<script>alert(1)</script>after");
        ASSERT(r != NULL, "script: root not NULL");
        HtmlNode *c = r->first_child;
        /* Should be text "after", not the script content */
        ASSERT(c != NULL, "script: has child");
        ASSERT(c->type == HTML_NODE_TEXT, "script: child is TEXT");
        ASSERT(strcmp(c->text, "after") == 0, "script: only text after script");
        html_node_free(r);
    }

    /* 17. <style> body not in tree */
    {
        HtmlNode *r = html_parse("<style>body{color:red}</style>text");
        ASSERT(r != NULL, "style: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c != NULL, "style: has child");
        ASSERT(c->type == HTML_NODE_TEXT, "style: child is TEXT");
        ASSERT(strcmp(c->text, "text") == 0, "style: only text after style");
        html_node_free(r);
    }

    /* 18. Bad close tag — no crash, best-effort */
    {
        HtmlNode *r = html_parse("<b>bold</x>after");
        ASSERT(r != NULL, "bad close: root not NULL");
        /* Just ensure no crash and root is valid */
        html_node_free(r);
    }

    /* 19. Nested elements */
    {
        HtmlNode *r = html_parse("<div><p><b>x</b></p></div>");
        ASSERT(r != NULL, "nested: root not NULL");
        HtmlNode *div = r->first_child;
        ASSERT(div && strcmp(div->tag, "div") == 0, "nested: div");
        HtmlNode *p = div->first_child;
        ASSERT(p && strcmp(p->tag, "p") == 0, "nested: p inside div");
        HtmlNode *b = p->first_child;
        ASSERT(b && strcmp(b->tag, "b") == 0, "nested: b inside p");
        HtmlNode *txt = b->first_child;
        ASSERT(txt && txt->type == HTML_NODE_TEXT, "nested: text inside b");
        ASSERT(strcmp(txt->text, "x") == 0, "nested: text content");
        html_node_free(r);
    }

    /* 20. html_node_free(NULL) — no crash */
    {
        html_node_free(NULL); /* must not crash */
        ASSERT(1, "html_node_free(NULL): no crash");
    }

    /* 21. html_attr_get on NULL — no crash */
    {
        const char *v = html_attr_get(NULL, "foo");
        ASSERT(v == NULL, "html_attr_get(NULL): returns NULL");
    }

    /* 22. Numeric entity in BMP (3-byte UTF-8): &#x2022; → U+2022 bullet */
    {
        RAII_HTML_NODE HtmlNode *r = html_parse("&#x2022;");
        ASSERT(r != NULL, "entity bullet hex: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "entity bullet hex: text node");
        /* U+2022 = E2 80 A2 in UTF-8 */
        ASSERT(c->text &&
               (unsigned char)c->text[0] == 0xE2 &&
               (unsigned char)c->text[1] == 0x80 &&
               (unsigned char)c->text[2] == 0xA2,
               "entity bullet hex: 3-byte UTF-8");
    }

    /* 23. Hex entity with uppercase A-F: &#xA0; → U+00A0 non-breaking space */
    {
        RAII_HTML_NODE HtmlNode *r = html_parse("&#xA0;");
        ASSERT(r != NULL, "entity nbsp hex upper: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "entity nbsp hex upper: text node");
        ASSERT(c->text &&
               (unsigned char)c->text[0] == 0xC2 &&
               (unsigned char)c->text[1] == 0xA0,
               "entity nbsp hex upper: UTF-8 correct");
    }

    /* 24. Hex entity with lowercase a-f: &#xa0; → U+00A0 */
    {
        RAII_HTML_NODE HtmlNode *r = html_parse("&#xa0;");
        ASSERT(r != NULL, "entity nbsp hex lower: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "entity nbsp hex lower: text node");
        ASSERT(c->text &&
               (unsigned char)c->text[0] == 0xC2 &&
               (unsigned char)c->text[1] == 0xA0,
               "entity nbsp hex lower: UTF-8 correct");
    }

    /* 25. 4-byte UTF-8 entity: &#x1F600; → U+1F600 emoji */
    {
        RAII_HTML_NODE HtmlNode *r = html_parse("&#x1F600;");
        ASSERT(r != NULL, "entity emoji: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "entity emoji: text node");
        /* U+1F600 = F0 9F 98 80 in UTF-8 */
        ASSERT(c->text &&
               (unsigned char)c->text[0] == 0xF0 &&
               (unsigned char)c->text[1] == 0x9F,
               "entity emoji: 4-byte UTF-8");
    }

    /* 26. Unknown entity: &unknown; → copied verbatim */
    {
        RAII_HTML_NODE HtmlNode *r = html_parse("&unknown;");
        ASSERT(r != NULL, "entity unknown: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "entity unknown: text node");
        ASSERT(c->text && c->text[0] == '&', "entity unknown: & preserved");
    }

    /* 27. Auto-close list items: <li>one<li>two triggers stk_pop */
    {
        RAII_HTML_NODE HtmlNode *r = html_parse("<ul><li>one<li>two</ul>");
        ASSERT(r != NULL, "auto-close li: root not NULL");
        HtmlNode *ul = r->first_child;
        ASSERT(ul && strcmp(ul->tag, "ul") == 0, "auto-close li: ul present");
        /* Should have two li children */
        int count = 0;
        HtmlNode *ch = ul->first_child;
        while (ch) { if (strcmp(ch->tag, "li") == 0) count++; ch = ch->next_sibling; }
        ASSERT(count == 2, "auto-close li: two li children");
    }

    /* 28. <!DOCTYPE html> → PS_DECL, no tree node created */
    {
        RAII_HTML_NODE HtmlNode *r = html_parse("<!DOCTYPE html>text");
        ASSERT(r != NULL, "doctype: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c != NULL, "doctype: has child");
        ASSERT(c->type == HTML_NODE_TEXT, "doctype: child is text");
        ASSERT(strcmp(c->text, "text") == 0, "doctype: only text after DOCTYPE");
    }

    /* 29. <script> with no closing tag — no crash, skip to end */
    {
        RAII_HTML_NODE HtmlNode *r = html_parse("<script>orphan");
        ASSERT(r != NULL, "script no close: root not NULL");
        /* No children (script content skipped, nothing after) */
    }

    /* 30. <br /> (space before /) triggers PS_ATTR_SEP self-close */
    {
        RAII_HTML_NODE HtmlNode *r = html_parse("<br />");
        ASSERT(r != NULL, "br space slash: root not NULL");
        HtmlNode *br = r->first_child;
        ASSERT(br && strcmp(br->tag, "br") == 0, "br space slash: tag is br");
        ASSERT(br->first_child == NULL, "br space slash: no children");
    }

    /* 31. Boolean attr followed by another attr: <input disabled class="x"> */
    {
        RAII_HTML_NODE HtmlNode *r = html_parse("<input disabled class=\"x\">");
        ASSERT(r != NULL, "bool attr space: root not NULL");
        HtmlNode *inp = r->first_child;
        ASSERT(inp != NULL, "bool attr space: input present");
        const char *dis = html_attr_get(inp, "disabled");
        const char *cls = html_attr_get(inp, "class");
        ASSERT(dis != NULL, "bool attr space: disabled present");
        ASSERT(cls && strcmp(cls, "x") == 0, "bool attr space: class is x");
    }

    /* 32. Unquoted attr followed by space+attr: <input type=text class="x"> */
    {
        RAII_HTML_NODE HtmlNode *r = html_parse("<input type=text class=\"x\">");
        ASSERT(r != NULL, "unquoted+space attr: root not NULL");
        HtmlNode *inp = r->first_child;
        ASSERT(inp != NULL, "unquoted+space attr: input present");
        const char *type = html_attr_get(inp, "type");
        const char *cls  = html_attr_get(inp, "class");
        ASSERT(type && strcmp(type, "text") == 0, "unquoted+space attr: type=text");
        ASSERT(cls  && strcmp(cls,  "x")    == 0, "unquoted+space attr: class=x");
    }

    /* 33. Malformed tag: < followed by non-alpha treated as text */
    {
        /* Starting with < so the entire result is one TEXT node */
        RAII_HTML_NODE HtmlNode *r = html_parse("<3 love");
        ASSERT(r != NULL, "malformed lt: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "malformed lt: text node");
        /* Content should include the < treated as text */
        ASSERT(strstr(c->text, "<") != NULL, "malformed lt: < preserved");
    }

    /* 34. Comment with single dash in content: <!-- foo - bar --> */
    {
        RAII_HTML_NODE HtmlNode *r = html_parse("<!-- foo - bar -->baz");
        ASSERT(r != NULL, "comment single dash: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "comment single dash: text node");
        ASSERT(strcmp(c->text, "baz") == 0, "comment single dash: only text after");
    }

    /* 35. <!- (single dash, no comment) → PS_DECL */
    {
        RAII_HTML_NODE HtmlNode *r = html_parse("<!-not-a-comment>text");
        ASSERT(r != NULL, "bang single dash: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "bang single dash: text node");
        ASSERT(strcmp(c->text, "text") == 0, "bang single dash: only text after");
    }

    /* 36. <!-- --x --> double-dash then non-> in comment → PS_CMT resumes */
    {
        RAII_HTML_NODE HtmlNode *r = html_parse("<!-- --x -->end");
        ASSERT(r != NULL, "comment double dash non-close: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "comment dd non-close: text node");
        ASSERT(strcmp(c->text, "end") == 0, "comment dd non-close: only text after");
    }

    /* 37. Attr name starting immediately after tag name (no space): PS_OPEN else branch */
    {
        /* "<div=text>" — 'd','i','v' collected in nb, then '=' is not alnum/-/_/:.
         * so done=1, cur=node_elem("div"), then c='=' → else branch line 342 */
        RAII_HTML_NODE HtmlNode *r = html_parse("<div=text>");
        ASSERT(r != NULL, "attr immediate: root not NULL");
        /* div element should exist as first child */
        HtmlNode *c = r->first_child;
        ASSERT(c != NULL, "attr immediate: has child");
        ASSERT(c->type == HTML_NODE_ELEMENT, "attr immediate: element node");
        ASSERT(strcmp(c->tag, "div") == 0, "attr immediate: tag is div");
    }

    /* 38b. Latin-1 named entities (aacute, eacute, uuml, ouml, uacute, iacute) */
    {
        /* &aacute; → á = U+00E1 = 0xC3 0xA1 in UTF-8 */
        RAII_HTML_NODE HtmlNode *r = html_parse("&aacute;");
        ASSERT(r != NULL, "latin1 aacute: root not NULL");
        HtmlNode *c = r->first_child;
        ASSERT(c && c->type == HTML_NODE_TEXT, "latin1 aacute: text node");
        ASSERT(c->text &&
               (unsigned char)c->text[0] == 0xC3 &&
               (unsigned char)c->text[1] == 0xA1,
               "latin1 aacute: UTF-8 C3 A1");
    }
    {
        /* &eacute; → é = U+00E9 = 0xC3 0xA9 */
        RAII_HTML_NODE HtmlNode *r = html_parse("&eacute;");
        HtmlNode *c = r ? r->first_child : NULL;
        ASSERT(c && c->text &&
               (unsigned char)c->text[0] == 0xC3 &&
               (unsigned char)c->text[1] == 0xA9,
               "latin1 eacute: UTF-8 C3 A9");
    }
    {
        /* &uuml; → ü = U+00FC = 0xC3 0xBC */
        RAII_HTML_NODE HtmlNode *r = html_parse("&uuml;");
        HtmlNode *c = r ? r->first_child : NULL;
        ASSERT(c && c->text &&
               (unsigned char)c->text[0] == 0xC3 &&
               (unsigned char)c->text[1] == 0xBC,
               "latin1 uuml: UTF-8 C3 BC");
    }
    {
        /* sentence with multiple Latin-1 entities */
        RAII_HTML_NODE HtmlNode *r = html_parse("Cs&aacute;sz&aacute;r");
        HtmlNode *c = r ? r->first_child : NULL;
        ASSERT(c && c->text && strstr(c->text, "á") != NULL,
               "latin1 multi: entities decoded in sentence");
    }

    /* 38. Codepoint > 0x10FFFF → cp_to_utf8 returns 0 (entity dropped) */
    {
        /* &#x200000; is 0x200000 > 0x10FFFF → cp_to_utf8 returns 0, entity skipped */
        RAII_HTML_NODE HtmlNode *r = html_parse("&#x200000;");
        ASSERT(r != NULL, "cp>0x10FFFF: root not NULL");
        /* Entity is dropped: text node has empty text or no children */
        HtmlNode *c = r->first_child;
        /* Either no child (empty text) or text is empty string */
        if (c) {
            ASSERT(c->type == HTML_NODE_TEXT, "cp>0x10FFFF: text node type");
            ASSERT(c->text != NULL && c->text[0] == '\0',
                   "cp>0x10FFFF: entity dropped → empty text");
        }
    }
}
