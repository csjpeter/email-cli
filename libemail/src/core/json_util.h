#ifndef JSON_UTIL_H
#define JSON_UTIL_H

/**
 * @file json_util.h
 * @brief Minimal JSON parser for Gmail API responses.
 *
 * Recursive descent parser that handles the JSON subset used by the Gmail
 * REST API: string/number values, arrays of strings, iteration over object
 * arrays.  Not a general-purpose JSON library — intentionally minimal.
 *
 * @note Unicode limitation: \\uXXXX escapes are decoded only for the ASCII
 *       range (U+0000–U+007F).  Non-ASCII codepoints (U+0080+), including
 *       BMP and surrogate pairs, are replaced with '?'.  This is acceptable
 *       because Gmail API returns UTF-8 text directly in JSON string values
 *       rather than \\uXXXX-encoding it; the escape form appears only for
 *       control characters and ASCII punctuation in practice.
 */

/**
 * @brief Extract a string value for a given key from a JSON object.
 *
 * Searches for `"key": "value"` at the top level of @p json.
 * Handles escape sequences (\\, \", \n, \t, \/, \uXXXX).
 * Skips nested objects and arrays when scanning for the key.
 *
 * @param json  NUL-terminated JSON string.
 * @param key   Key name to search for (without quotes).
 * @return Heap-allocated unescaped value, or NULL if not found.
 *         Caller must free().
 */
char *json_get_string(const char *json, const char *key);

/**
 * @brief Extract an integer value for a given key from a JSON object.
 *
 * Searches for `"key": 123` at the top level of @p json.
 *
 * @param json  NUL-terminated JSON string.
 * @param key   Key name to search for.
 * @param out   On success, set to the integer value.
 * @return 0 on success, -1 if not found or not a number.
 */
int json_get_int(const char *json, const char *key, int *out);

/**
 * @brief Extract an array of strings for a given key.
 *
 * Searches for `"key": ["a", "b", "c"]` at the top level of @p json.
 *
 * @param json      NUL-terminated JSON string.
 * @param key       Key name to search for.
 * @param out       On success, set to heap-allocated array of heap-allocated
 *                  strings.  Caller frees each element and the array.
 * @param count_out Set to the number of strings returned.
 * @return 0 on success (including empty array), -1 if key not found.
 */
int json_get_string_array(const char *json, const char *key,
                          char ***out, int *count_out);

/**
 * @brief Callback type for json_foreach_object().
 *
 * @param obj_json  Pointer into the original JSON at the start of the
 *                  current object ('{').  The object is NUL-terminated in
 *                  a temporary buffer so the callback can call json_get_*
 *                  on it safely.
 * @param index     Zero-based index of the current object in the array.
 * @param ctx       User-supplied context pointer.
 */
typedef void (*JsonObjectCb)(const char *obj_json, int index, void *ctx);

/**
 * @brief Iterate over an array of objects for a given key.
 *
 * For `"key": [{...}, {...}]`, calls @p cb once per object with a
 * NUL-terminated copy of that object's JSON.
 *
 * @param json  NUL-terminated JSON string.
 * @param key   Key name to search for.
 * @param cb    Callback invoked for each object.
 * @param ctx   Passed through to @p cb.
 * @return Number of objects iterated, or -1 if key not found.
 */
int json_foreach_object(const char *json, const char *key,
                        JsonObjectCb cb, void *ctx);

#endif /* JSON_UTIL_H */
