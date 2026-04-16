#include "test_helpers.h"
#include "json_util.h"
#include <stdlib.h>
#include <string.h>

/* ── json_get_string ────────────────────────────────────────────────── */

static void test_json_get_string_simple(void) {
    const char *j = "{\"id\": \"abc123\"}";
    char *v = json_get_string(j, "id");
    ASSERT(v != NULL, "json_get_string: found");
    ASSERT(strcmp(v, "abc123") == 0, "json_get_string: value matches");
    free(v);
}

static void test_json_get_string_second_key(void) {
    const char *j = "{\"id\": \"abc\", \"name\": \"hello\"}";
    char *v = json_get_string(j, "name");
    ASSERT(v != NULL, "json_get_string second key: found");
    ASSERT(strcmp(v, "hello") == 0, "json_get_string second key: value");
    free(v);
}

static void test_json_get_string_not_found(void) {
    const char *j = "{\"id\": \"abc\"}";
    char *v = json_get_string(j, "missing");
    ASSERT(v == NULL, "json_get_string not found: returns NULL");
}

static void test_json_get_string_escape(void) {
    const char *j = "{\"msg\": \"hello\\nworld\\t!\"}";
    char *v = json_get_string(j, "msg");
    ASSERT(v != NULL, "json_get_string escape: found");
    ASSERT(strcmp(v, "hello\nworld\t!") == 0, "json_get_string escape: unescaped");
    free(v);
}

static void test_json_get_string_escaped_quote(void) {
    const char *j = "{\"msg\": \"say \\\"hi\\\"\"}";
    char *v = json_get_string(j, "msg");
    ASSERT(v != NULL, "json_get_string escaped quote: found");
    ASSERT(strcmp(v, "say \"hi\"") == 0, "json_get_string escaped quote: value");
    free(v);
}

static void test_json_get_string_skip_nested_object(void) {
    const char *j = "{\"a\": {\"x\": 1}, \"b\": \"found\"}";
    char *v = json_get_string(j, "b");
    ASSERT(v != NULL, "json skip nested obj: found");
    ASSERT(strcmp(v, "found") == 0, "json skip nested obj: value");
    free(v);
}

static void test_json_get_string_skip_nested_array(void) {
    const char *j = "{\"arr\": [1, 2, 3], \"key\": \"val\"}";
    char *v = json_get_string(j, "key");
    ASSERT(v != NULL, "json skip nested array: found");
    ASSERT(strcmp(v, "val") == 0, "json skip nested array: value");
    free(v);
}

static void test_json_get_string_null_input(void) {
    ASSERT(json_get_string(NULL, "k") == NULL, "json NULL json");
    ASSERT(json_get_string("{}", NULL) == NULL, "json NULL key");
}

static void test_json_get_string_empty_object(void) {
    ASSERT(json_get_string("{}", "k") == NULL, "json empty object");
}

static void test_json_get_string_whitespace(void) {
    const char *j = " {  \"id\"  :  \"val\"  } ";
    char *v = json_get_string(j, "id");
    ASSERT(v != NULL, "json whitespace: found");
    ASSERT(strcmp(v, "val") == 0, "json whitespace: value");
    free(v);
}

static void test_json_get_string_empty_value(void) {
    const char *j = "{\"e\": \"\"}";
    char *v = json_get_string(j, "e");
    ASSERT(v != NULL, "json empty value: found");
    ASSERT(v[0] == '\0', "json empty value: empty string");
    free(v);
}

static void test_json_get_string_unicode_escape(void) {
    const char *j = "{\"u\": \"\\u0041\"}";
    char *v = json_get_string(j, "u");
    ASSERT(v != NULL, "json unicode escape: found");
    ASSERT(v[0] == 'A' && v[1] == '\0', "json unicode escape: A");
    free(v);
}

static void test_json_get_string_backslash(void) {
    const char *j = "{\"p\": \"C:\\\\Users\"}";
    char *v = json_get_string(j, "p");
    ASSERT(v != NULL, "json backslash: found");
    ASSERT(strcmp(v, "C:\\Users") == 0, "json backslash: value");
    free(v);
}

/* ── json_get_int ───────────────────────────────────────────────────── */

static void test_json_get_int_simple(void) {
    const char *j = "{\"count\": 42}";
    int v = 0;
    ASSERT(json_get_int(j, "count", &v) == 0, "json_get_int: found");
    ASSERT(v == 42, "json_get_int: value");
}

static void test_json_get_int_negative(void) {
    const char *j = "{\"val\": -7}";
    int v = 0;
    ASSERT(json_get_int(j, "val", &v) == 0, "json_get_int negative: found");
    ASSERT(v == -7, "json_get_int negative: value");
}

static void test_json_get_int_quoted(void) {
    const char *j = "{\"n\": \"123\"}";
    int v = 0;
    ASSERT(json_get_int(j, "n", &v) == 0, "json_get_int quoted: found");
    ASSERT(v == 123, "json_get_int quoted: value");
}

static void test_json_get_int_not_found(void) {
    const char *j = "{\"a\": 1}";
    int v = 0;
    ASSERT(json_get_int(j, "b", &v) == -1, "json_get_int not found");
}

static void test_json_get_int_null_input(void) {
    int v = 0;
    ASSERT(json_get_int(NULL, "k", &v) == -1, "json_get_int NULL");
}

/* ── json_get_string_array ──────────────────────────────────────────── */

static void test_json_string_array_simple(void) {
    const char *j = "{\"labels\": [\"INBOX\", \"Work\", \"STARRED\"]}";
    char **arr = NULL; int count = 0;
    ASSERT(json_get_string_array(j, "labels", &arr, &count) == 0, "str_array: ok");
    ASSERT(count == 3, "str_array: count=3");
    ASSERT(strcmp(arr[0], "INBOX") == 0, "str_array[0]=INBOX");
    ASSERT(strcmp(arr[1], "Work") == 0, "str_array[1]=Work");
    ASSERT(strcmp(arr[2], "STARRED") == 0, "str_array[2]=STARRED");
    for (int i = 0; i < count; i++) free(arr[i]);
    free(arr);
}

static void test_json_string_array_empty(void) {
    const char *j = "{\"labels\": []}";
    char **arr = NULL; int count = 0;
    ASSERT(json_get_string_array(j, "labels", &arr, &count) == 0, "str_array empty: ok");
    ASSERT(count == 0, "str_array empty: count=0");
    free(arr);
}

static void test_json_string_array_single(void) {
    const char *j = "{\"labels\": [\"INBOX\"]}";
    char **arr = NULL; int count = 0;
    ASSERT(json_get_string_array(j, "labels", &arr, &count) == 0, "str_array single: ok");
    ASSERT(count == 1, "str_array single: count=1");
    ASSERT(strcmp(arr[0], "INBOX") == 0, "str_array single: INBOX");
    free(arr[0]); free(arr);
}

static void test_json_string_array_not_found(void) {
    const char *j = "{\"x\": 1}";
    char **arr = NULL; int count = 0;
    ASSERT(json_get_string_array(j, "labels", &arr, &count) == -1, "str_array missing: -1");
}

static void test_json_string_array_with_escapes(void) {
    const char *j = "{\"v\": [\"a\\nb\", \"c\\td\"]}";
    char **arr = NULL; int count = 0;
    ASSERT(json_get_string_array(j, "v", &arr, &count) == 0, "str_array escape: ok");
    ASSERT(count == 2, "str_array escape: count=2");
    ASSERT(strcmp(arr[0], "a\nb") == 0, "str_array escape[0]");
    ASSERT(strcmp(arr[1], "c\td") == 0, "str_array escape[1]");
    for (int i = 0; i < count; i++) free(arr[i]);
    free(arr);
}

/* ── json_foreach_object ────────────────────────────────────────────── */

struct obj_ctx {
    char ids[16][32];
    int count;
};

static void collect_id(const char *obj, int index, void *ctx) {
    struct obj_ctx *c = ctx;
    (void)index;
    char *id = json_get_string(obj, "id");
    if (id && c->count < 16) {
        strncpy(c->ids[c->count], id, 31);
        c->ids[c->count][31] = '\0';
        c->count++;
    }
    free(id);
}

static void test_json_foreach_simple(void) {
    const char *j = "{\"messages\": [{\"id\": \"abc\"}, {\"id\": \"def\"}]}";
    struct obj_ctx ctx = {.count = 0};
    int n = json_foreach_object(j, "messages", collect_id, &ctx);
    ASSERT(n == 2, "foreach: 2 objects");
    ASSERT(ctx.count == 2, "foreach: collected 2");
    ASSERT(strcmp(ctx.ids[0], "abc") == 0, "foreach[0]=abc");
    ASSERT(strcmp(ctx.ids[1], "def") == 0, "foreach[1]=def");
}

static void test_json_foreach_empty(void) {
    const char *j = "{\"messages\": []}";
    struct obj_ctx ctx = {.count = 0};
    int n = json_foreach_object(j, "messages", collect_id, &ctx);
    ASSERT(n == 0, "foreach empty: 0");
    ASSERT(ctx.count == 0, "foreach empty: none collected");
}

static void test_json_foreach_not_found(void) {
    const char *j = "{\"x\": 1}";
    struct obj_ctx ctx = {.count = 0};
    int n = json_foreach_object(j, "messages", collect_id, &ctx);
    ASSERT(n == -1, "foreach missing: -1");
}

static void test_json_foreach_nested_objects(void) {
    const char *j = "{\"items\": [{\"id\": \"a\", \"meta\": {\"x\": 1}}, {\"id\": \"b\"}]}";
    struct obj_ctx ctx = {.count = 0};
    int n = json_foreach_object(j, "items", collect_id, &ctx);
    ASSERT(n == 2, "foreach nested: 2");
    ASSERT(strcmp(ctx.ids[0], "a") == 0, "foreach nested[0]=a");
    ASSERT(strcmp(ctx.ids[1], "b") == 0, "foreach nested[1]=b");
}

static void test_json_foreach_single(void) {
    const char *j = "{\"items\": [{\"id\": \"only\"}]}";
    struct obj_ctx ctx = {.count = 0};
    int n = json_foreach_object(j, "items", collect_id, &ctx);
    ASSERT(n == 1, "foreach single: 1");
    ASSERT(strcmp(ctx.ids[0], "only") == 0, "foreach single[0]=only");
}

/* ── Registration ───────────────────────────────────────────────────── */

void run_json_util_tests(void) {
    /* json_get_string */
    RUN_TEST(test_json_get_string_simple);
    RUN_TEST(test_json_get_string_second_key);
    RUN_TEST(test_json_get_string_not_found);
    RUN_TEST(test_json_get_string_escape);
    RUN_TEST(test_json_get_string_escaped_quote);
    RUN_TEST(test_json_get_string_skip_nested_object);
    RUN_TEST(test_json_get_string_skip_nested_array);
    RUN_TEST(test_json_get_string_null_input);
    RUN_TEST(test_json_get_string_empty_object);
    RUN_TEST(test_json_get_string_whitespace);
    RUN_TEST(test_json_get_string_empty_value);
    RUN_TEST(test_json_get_string_unicode_escape);
    RUN_TEST(test_json_get_string_backslash);

    /* json_get_int */
    RUN_TEST(test_json_get_int_simple);
    RUN_TEST(test_json_get_int_negative);
    RUN_TEST(test_json_get_int_quoted);
    RUN_TEST(test_json_get_int_not_found);
    RUN_TEST(test_json_get_int_null_input);

    /* json_get_string_array */
    RUN_TEST(test_json_string_array_simple);
    RUN_TEST(test_json_string_array_empty);
    RUN_TEST(test_json_string_array_single);
    RUN_TEST(test_json_string_array_not_found);
    RUN_TEST(test_json_string_array_with_escapes);

    /* json_foreach_object */
    RUN_TEST(test_json_foreach_simple);
    RUN_TEST(test_json_foreach_empty);
    RUN_TEST(test_json_foreach_not_found);
    RUN_TEST(test_json_foreach_nested_objects);
    RUN_TEST(test_json_foreach_single);
}
