#ifndef HTML_PARSER_H
#define HTML_PARSER_H

/**
 * @file html_parser.h
 * @brief Minimal HTML parser: tokenizer + DOM-like tree builder.
 *
 * Produces a tree of HtmlNode objects for the html_render() traversal.
 * Best-effort: malformed HTML is recovered gracefully (no crash).
 */

typedef struct HtmlAttr {
    char            *name;
    char            *value;
    struct HtmlAttr *next;
} HtmlAttr;

typedef enum {
    HTML_NODE_ELEMENT,
    HTML_NODE_TEXT
} HtmlNodeType;

typedef struct HtmlNode {
    HtmlNodeType     type;
    char            *tag;           /**< lowercase tag name; NULL for text nodes */
    char            *text;          /**< decoded text content; NULL for elements */
    HtmlAttr        *attrs;         /**< linked list; NULL if no attributes */
    struct HtmlNode *first_child;
    struct HtmlNode *next_sibling;
    struct HtmlNode *parent;
} HtmlNode;

/**
 * @brief Parses an HTML string into a tree.
 *
 * Returns a synthetic root node (tag "__root__") whose children are the
 * top-level nodes of the document.  Returns NULL if html is NULL.
 * Caller must call html_node_free(root) when done.
 */
HtmlNode *html_parse(const char *html);

/**
 * @brief Frees an entire subtree.  Safe to call with NULL.
 */
void html_node_free(HtmlNode *node);

/**
 * @brief Returns the value of the first attribute matching name, or NULL.
 */
const char *html_attr_get(const HtmlNode *node, const char *name);

#endif /* HTML_PARSER_H */
