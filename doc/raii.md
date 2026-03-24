# Resource Management with GNU RAII

## The Challenge
Manual memory and resource management in C is prone to leaks, especially when functions have multiple exit points (e.g., error handling).

## The Solution: `__attribute__((cleanup))`
`email-cli` leverages GCC's `cleanup` attribute to implement RAII (Resource Acquisition Is Initialization).

### Implementation (`src/core/raii.h`)
We define macros that bind variables to cleanup functions:
- `RAII_STRING`: Calls `free()` on a `char*`.
- `RAII_CURL`: Calls `curl_easy_cleanup()` on a `CURL*`.
- `RAII_FILE`: Calls `fclose()` on a `FILE*`.

### Example Usage
```c
void process_email() {
    RAII_STRING char *buffer = malloc(1024);
    RAII_CURL CURL *curl = curl_easy_init();
    
    if (!curl) return; // 'buffer' is automatically freed here!
    
    // Do work...
} // Both 'curl' and 'buffer' are automatically cleaned up here!
```

## Benefits
1. **No "Goto Cleanup":** Eliminates boilerplate code at every return statement.
2. **Exception-Like Safety:** Guaranteed cleanup even on early returns.
3. **Cleaner Logic:** Developers can focus on the "Happy Path" logic.
