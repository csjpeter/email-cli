#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-22
# Tests that the sanitizer and Valgrind verdicts of manage.sh catch a real fault.

# EMAIL-224: `manage.sh sanitize` and `memcheck` turn the logs of an
# instrumented run into a verdict (tests/sanitize/scan_logs.sh) and start every
# program through tests/sanitize/valgrind-program.sh.  Here a program with a
# deliberate leak and one with a heap overflow are built in a scratch directory
# under build/ and put through the same two mechanisms: the verdict must be
# "fail" with the log named, and a clean program must pass.
set -u
cd "$(dirname "$(readlink -f "$0")")/../.." || exit 1
ROOT="$PWD"
WORK="$ROOT/build/sanitize-test-$$"
PASSED=0; FAILED=0
ok()  { echo "  [PASS] $1"; PASSED=$((PASSED + 1)); }
bad() { echo "  [FAIL] $1"; FAILED=$((FAILED + 1)); }
SCAN="$ROOT/tests/sanitize/scan_logs.sh"
mkdir -p "$WORK/src" "$WORK/asan-clean" "$WORK/asan-leak" "$WORK/asan-overflow" \
         "$WORK/vg-bin" "$WORK/vg-clean" "$WORK/vg-leak" "$WORK/scan" || exit 1

cat > "$WORK/src/clean.c" <<'C'
#include <stdlib.h>
#include <string.h>
int main(void) { char *p = malloc(8); if (!p) return 1; strcpy(p, "ok"); free(p); return 0; }
C
cat > "$WORK/src/leak.c" <<'C'
#include <stdlib.h>
#include <string.h>
int main(void) { char *p = malloc(64); if (!p) return 1; strcpy(p, "leaked"); p = NULL; return 0; }
C
cat > "$WORK/src/overflow.c" <<'C'
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    char *p = malloc(8); if (!p) return 1;
    memset(p, 'x', 8 + (size_t)argc);   /* writes past the end */
    int r = p[(int)strlen(argv[0]) % 8]; free(p); return r == 0; }
C

# 1. help lists both verbs.
out=$("$ROOT/manage.sh" help)
echo "$out" | grep -q '^  sanitize '     && ok "1.1 help lists sanitize"     || bad "1.1 help has no sanitize"
echo "$out" | grep -q '^  memcheck ' && ok "1.2 help lists memcheck" || bad "1.2 help has no memcheck"

# 2. the sanitizer verdict: an empty log directory passes, any file fails and is named.
out=$("$SCAN" sanitizer "$WORK/scan" 2>&1); rc=$?
[ "$rc" -eq 0 ] && ok "2.1 an empty log directory passes" || bad "2.1 rc=$rc: $out"
printf '==1==ERROR: AddressSanitizer: heap-buffer-overflow\n' > "$WORK/scan/asan.1"
out=$("$SCAN" sanitizer "$WORK/scan" 2>&1); rc=$?
[ "$rc" -eq 1 ] && echo "$out" | grep -q 'asan.1' && ok "2.2 a log file fails the run and is named" || bad "2.2 rc=$rc: $out"
"$SCAN" sanitizer "$WORK/no-such-dir" 2>"$WORK/err.txt"; rc=$?
[ "$rc" -eq 2 ] && [ -s "$WORK/err.txt" ] && ok "2.3 a missing directory is a usage error" || bad "2.3 rc=$rc"

# 3. ASan: a clean program leaves no log; a leak and an overflow leave one.
gcc -g -fsanitize=address -fno-omit-frame-pointer "$WORK/src/clean.c" -o "$WORK/clean-asan" || bad "3.0 build clean"
gcc -g -fsanitize=address -fno-omit-frame-pointer "$WORK/src/leak.c" -o "$WORK/leak-asan" || bad "3.0 build leak"
gcc -g -fsanitize=address -fno-omit-frame-pointer "$WORK/src/overflow.c" -o "$WORK/overflow-asan" || bad "3.0 build overflow"
ASAN_OPTIONS="log_path=$WORK/asan-clean/asan:detect_leaks=1" "$WORK/clean-asan"; rc=$?
out=$("$SCAN" sanitizer "$WORK/asan-clean" 2>&1); src=$?
[ "$rc" -eq 0 ] && [ "$src" -eq 0 ] && ok "3.1 a clean program passes under ASan" || bad "3.1 rc=$rc scan=$src: $out"
ASAN_OPTIONS="log_path=$WORK/asan-leak/asan:detect_leaks=1" "$WORK/leak-asan"; rc=$?
out=$("$SCAN" sanitizer "$WORK/asan-leak" 2>&1); src=$?
[ "$rc" -ne 0 ] && [ "$src" -eq 1 ] && echo "$out" | grep -q 'LeakSanitizer' && echo "$out" | grep -q 'asan-leak/asan\.' \
    && ok "3.2 a deliberate leak fails and its log is named" || bad "3.2 rc=$rc scan=$src: $out"
ASAN_OPTIONS="log_path=$WORK/asan-overflow/asan:detect_leaks=1" "$WORK/overflow-asan"; rc=$?
out=$("$SCAN" sanitizer "$WORK/asan-overflow" 2>&1); src=$?
[ "$rc" -ne 0 ] && [ "$src" -eq 1 ] && echo "$out" | grep -q 'heap-buffer-overflow' \
    && ok "3.3 a deliberate overflow fails and its log is named" || bad "3.3 rc=$rc scan=$src: $out"

# 4. Valgrind through the wrapper that memcheck copies beside each program.
gcc -g -O0 "$WORK/src/clean.c" -o "$WORK/vg-bin/clean.bin" || bad "4.0 build clean"
gcc -g -O0 "$WORK/src/leak.c"  -o "$WORK/vg-bin/leak.bin"  || bad "4.0 build leak"
cp "$ROOT/tests/sanitize/valgrind-program.sh" "$WORK/vg-bin/clean" || bad "4.0 copy wrapper"
cp "$ROOT/tests/sanitize/valgrind-program.sh" "$WORK/vg-bin/leak"  || bad "4.0 copy wrapper"
VG_LOG_DIR="$WORK/vg-clean" VG_TEST_NAME=t1 "$WORK/vg-bin/clean"; rc=$?
out=$("$SCAN" valgrind "$WORK/vg-clean" 2>&1); src=$?
ls "$WORK/vg-clean"/t1-clean.*.log > /dev/null || bad "4.1 no log named after the test and program"
[ "$rc" -eq 0 ] && [ "$src" -eq 0 ] && ok "4.1 a clean program passes under the wrapper" || bad "4.1 rc=$rc scan=$src: $out"
VG_LOG_DIR="$WORK/vg-leak" VG_TEST_NAME=t2 "$WORK/vg-bin/leak"; rc=$?
out=$("$SCAN" valgrind "$WORK/vg-leak" 2>&1); src=$?
[ "$rc" -eq 1 ] && ok "4.2 the wrapped leaking program exits 1 (--error-exitcode)" || bad "4.2 rc=$rc"
[ "$src" -eq 1 ] && echo "$out" | grep -q 't2-leak\.[0-9]*\.log' && echo "$out" | grep -q 'definitely lost' \
    && ok "4.3 the leak fails the verdict and its log is named" || bad "4.3 scan=$src: $out"
VG_LOG_DIR= "$WORK/vg-bin/clean" 2>"$WORK/err.txt"; rc=$?
[ "$rc" -ne 0 ] && grep -q 'VG_LOG_DIR' "$WORK/err.txt" && ok "4.4 no VG_LOG_DIR is an error that names it" || bad "4.4 rc=$rc"

# 5. the suppression file: every entry is commented and none is for this project.
SUPP="$ROOT/tests/sanitize/valgrind.supp"
entries=$(grep -c '^{' "$SUPP")
commented=$(awk '/^#/ { c = 1; next } /^\{/ { if (c) n++; c = 0; next } { c = 0 } END { print n + 0 }' "$SUPP")
[ "$entries" -eq "$commented" ] && ok "5.1 every suppression entry has a comment above it ($entries entries)" || bad "5.1 $entries entries, $commented commented"
grep -v '^#' "$SUPP" | grep -E 'fun:(email|emc|local_|mime_|tui_)|obj:.*/(email-|libemail|libtuikit)' > "$WORK/own.txt"
[ ! -s "$WORK/own.txt" ] && ok "5.2 no entry names the project's own code" || bad "5.2 own code suppressed: $(cat "$WORK/own.txt")"

# 6. the verbs run the instrumented programs through the shared variables.
grep -q 'EMAIL_TEST_BIN_DIR' "$ROOT/tests/functional/run_functional.sh" \
    && grep -q 'EMAIL_TEST_BIN_DIR' "$ROOT/tests/integration/run_local_dovecot.sh" \
    && grep -q 'EMAIL_TEST_BIN_DIR' "$ROOT/tests/integration/run_integration.sh" \
    && ok "6.1 functional and integration suites take the programs from EMAIL_TEST_BIN_DIR" || bad "6.1 a suite ignores EMAIL_TEST_BIN_DIR"
grep -q 'log_path=\$SAN_DIR' "$ROOT/manage.sh" && grep -q 'halt_on_error=1' "$ROOT/manage.sh" \
    && ok "6.2 sanitize writes its logs under build/ and halts UBSan on the first error" || bad "6.2 sanitize options missing"
grep -q 'EMAIL_TEST_WAIT_SCALE' "$ROOT/manage.sh" && grep -q 'getenv("EMAIL_TEST_WAIT_SCALE")' "$ROOT/tests/pty/wait_scale.h" \
    && ok "6.3 memcheck stretches the waits of the PTY tests" || bad "6.3 no wait scale for Valgrind"
unscaled=$(grep -l '^#define WAIT_MS' "$ROOT"/tests/pty/*.c | xargs grep -L 'wait_scale()')
[ -z "$unscaled" ] && ok "6.4 every PTY test file scales its waits" || bad "6.4 unscaled: $unscaled"

case "$WORK" in
    "$ROOT"/build/sanitize-test-*) ;;
    *) echo "ERROR: refusing unsafe path '$WORK'" >&2; exit 1 ;;
esac
rm -rf "$ROOT/build/sanitize-test-${WORK##*/sanitize-test-}"
echo "Passed: $PASSED / $((PASSED + FAILED))"
[ "$FAILED" -eq 0 ]
