#ifndef RAII_H
#define RAII_H

#include <stdlib.h>
#include <stdio.h>
#include <curl/curl.h>

/**
 * Cleanup functions for GNU RAII attributes.
 * These are called when a variable with the __attribute__((cleanup(func))) goes out of scope.
 */

static inline void free_ptr(void *ptr) {
    void **p = (void **)ptr;
    if (*p) {
        free(*p);
        *p = NULL;
    }
}

static inline void curl_cleanup_ptr(void *ptr) {
    CURL **p = (CURL **)ptr;
    if (*p) {
        curl_easy_cleanup(*p);
        *p = NULL;
    }
}

static inline void curl_slist_free_all_ptr(void *ptr) {
    struct curl_slist **p = (struct curl_slist **)ptr;
    if (*p) {
        curl_slist_free_all(*p);
        *p = NULL;
    }
}

static inline void fclose_ptr(void *ptr) {
    FILE **p = (FILE **)ptr;
    if (*p) {
        fclose(*p);
        *p = NULL;
    }
}

#define RAII_STRING __attribute__((cleanup(free_ptr)))
#define RAII_CURL __attribute__((cleanup(curl_cleanup_ptr)))
#define RAII_SLIST __attribute__((cleanup(curl_slist_free_all_ptr)))
#define RAII_FILE __attribute__((cleanup(fclose_ptr)))

#endif // RAII_H
