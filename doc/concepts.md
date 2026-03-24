# Resource Management in C with RAII

## The Concept
Resource Acquisition Is Initialization (RAII) is a common pattern in languages like C++. It ensures that resources (memory, file handles, network sockets) are automatically released when they go out of scope.

In Standard C, this is usually handled manually with `goto cleanup` or nested `if` blocks, which is error-prone.

## GNU C Extension: `__attribute__((cleanup))`
GCC (and Clang) provides a powerful extension to implement RAII-like behavior:
`__attribute__((cleanup(function_name)))`.

When a variable with this attribute goes out of scope, the specified function is automatically called with a pointer to that variable.

### Example: File RAII
```c
void close_file(FILE **f) {
    if (*f) {
        fclose(*f);
        printf("File closed automatically.\n");
    }
}

void process() {
    FILE *my_file __attribute__((cleanup(close_file))) = fopen("data.txt", "r");
    if (!my_file) return;
    // Do work... no fclose() needed!
}
```

## Benefits in this Project
1.  **Safety:** CURL handles and malloc'd strings are guaranteed to be cleaned up, even on early returns.
2.  **Clean Code:** Functions are shorter and focus on the "Happy Path" without boilerplate cleanup logic at every exit point.
3.  **Correctness:** Prevents memory leaks and handle exhaustion.

## Implementation in `raii.h`
We define generic cleanup functions for common types:
- `CURL*` -> `curl_easy_cleanup`
- `char*` -> `free`
- `struct curl_slist*` -> `curl_slist_free_all`
