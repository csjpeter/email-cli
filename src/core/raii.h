#ifndef RAII_H
#define RAII_H

#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>

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

static inline void fclose_ptr(void *ptr) {
    FILE **p = (FILE **)ptr;
    if (*p) {
        fclose(*p);
        *p = NULL;
    }
}

static inline void closedir_ptr(DIR **p) {
    if (p && *p) {
        closedir(*p);
        *p = NULL;
    }
}

#define RAII_STRING __attribute__((cleanup(free_ptr)))
#define RAII_FILE __attribute__((cleanup(fclose_ptr)))
#define RAII_DIR __attribute__((cleanup(closedir_ptr)))

/* To avoid circular dependencies with Config, we use a generic cleanup for it 
 * but it must be defined in each file that uses it or we use a macro. */
#define RAII_WITH_CLEANUP(func) __attribute__((cleanup(func)))

#endif // RAII_H
