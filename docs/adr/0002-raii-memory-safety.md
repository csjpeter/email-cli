# ADR-0002: RAII Memory Safety via GNU cleanup Attribute

**Status:** Accepted

## Context

Manual `free()` / `fclose()` / `curl_easy_cleanup()` at every return point leads
to leaks when error paths are added or refactored. `goto cleanup` patterns work
but clutter the happy-path logic.

## Decision

All resource-owning variables use GCC/Clang's `__attribute__((cleanup(fn)))` to
bind a cleanup function that fires automatically when the variable goes out of scope.
This is implemented in `src/core/raii.h` as four macros:

| Macro | Type | Cleanup called |
|-------|------|----------------|
| `RAII_STRING` | `char *` | `free()` |
| `RAII_CURL` | `CURL *` | `curl_easy_cleanup()` |
| `RAII_FILE` | `FILE *` | `fclose()` |
| `RAII_SLIST` | `struct curl_slist *` | `curl_slist_free_all()` |

### Usage example

```c
int fetch(const Config *cfg) {
    RAII_CURL  CURL *curl = curl_adapter_init(cfg->user, cfg->pass, 1);
    RAII_STRING char *url  = NULL;

    if (!curl) return -1;                // curl cleaned up automatically
    if (asprintf(&url, "%s/%s", ...) == -1) return -1;  // url too

    return curl_adapter_fetch(curl, url, NULL, write_to_stdout);
}   // both freed here, no matter which return path was taken
```

## Consequences

- No `goto cleanup` patterns anywhere in the codebase.
- Adding an early return never risks a resource leak — the compiler enforces it.
- Requires GCC or Clang (`-std=c11` + GNU extensions via `-D_GNU_SOURCE`).
  MSVC is not supported; this is a deliberate trade-off for a Linux-targeted tool.
- New resource types must get a cleanup function and macro added to `raii.h`.
