# Unit Testing Methodology

## Philosophy
We avoid external testing frameworks to keep dependencies minimal and strictly C-based.

## Custom Test Engine
Located in `tests/unit/test_runner.c`, our engine uses simple macros:
- `ASSERT(cond, msg)`: Increments run count and failure count if condition is false.
- `RUN_TEST(func)`: Executes a test function and prints its status.

## Diagnostic Support
Tests are designed to run under multiple diagnostic tools:

### 1. Address Sanitizer (ASAN)
Compile with `-fsanitize=address`. This detects:
- Out-of-bounds access.
- Use-after-free.
- Memory leaks (at exit).

### 2. Valgrind
Run the test binary with `valgrind --leak-check=full`. This provides deeper analysis of uninitialized memory and complex leaks.

### 3. GCOV/LCOV
Generate coverage reports to identify untested code paths.
```bash
make coverage
```
The goal is >90% coverage for the `core` and `infrastructure` layers.
