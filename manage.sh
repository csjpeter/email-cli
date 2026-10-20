#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-03-24
# Build, test, package and install management script of email-cli.

# email-cli management script
# A user-friendly interface for building, testing, and running the project

set -e

PROJECT_NAME="email-cli"
BUILD_DIR="./build"
BIN_DIR="./bin"
BIN_PATH="$BIN_DIR/$PROJECT_NAME"

INSTALL_DIR="${HOME}/.local/bin"
INSTALL_BINS="email-cli email-cli-ro email-sync email-tui email-import-rules"

show_help() {
    echo "Usage: ./manage.sh [command]"
    echo ""
    echo "Commands:"
    echo "  deps           Install system dependencies (supports Ubuntu 24.04, Rocky 9)"
    echo "  build          Build the project in Release mode"
    echo "  debug          Build the project in Debug mode (with ASAN)"
    echo "  run            Build and run the application"
    echo "  test [FILTER]  Build and run unit tests (with ASAN); FILTER runs only the"
    echo "                 test suites whose name contains it"
    echo "  functional [PHASES]  Build and run the functional test suite; PHASES is a list"
    echo "                 such as 7,60-62 (see tests/functional/run_functional.sh --list)"
    echo "  pty [SUITE [FILTER]]  Build and run the PTY (terminal) test suites; SUITE is one"
    echo "                 of views gmail-tui mail-rules compose-dialog attachment"
    echo "                 send-local input-line compose screens, FILTER selects tests"
    echo "                 by name"
    echo "  valgrind       Build and run unit tests with Valgrind"
    echo "  headers [DIR]  Fail on a source file or script without the source header"
    echo "  names [DIR]    Fail when a tracked file names the owner's private domain"
    echo "  declared       Fail when a declared test is not built or a built test is not declared"
    echo "  check          Run every gate (headers, names, declared, test, functional, pty, valgrind) and summarise"
    echo "  coverage       Run tests and generate coverage report"
    echo "  integration    Run integration test against Dovecot IMAP container"
    echo "  integration-local  Run APPEND integration test with local Dovecot (no Docker)"
    echo "  e2e            Build the package, run tests/e2e in LXC containers (sudo lxc-*)"
    echo "  imap-down      Stop integration test container (preserves emails volume)"
    echo "  imap-clean     Remove integration test container and volume"
    echo "  install        Build (release) and install binaries to ~/.local/bin"
    echo "  uninstall      Remove installed binaries from ~/.local/bin"
    echo "  package [deb|rpm|all]  Build release and create package(s) in build/packages/"
    echo "  timing [KEY-N] Print the Gate line from build/timing.log; with a key, comment it on the ticket"
    echo "  clean-logs     Purge all application log files"
    echo "  clean          Remove all build artifacts"
    echo "  help           Show this help message"
    echo ""
    echo "Environment:"
    echo "  WORKSPACE_LIBS=bundled|system|auto  how libtuikit and libptytest come in:"
    echo "                 bundled = git submodules under deps/, static (default);"
    echo "                 system = installed -dev packages; auto = installed if present"
}

# Every source file and script starts with the header of the source-header
# policy: an SPDX licence tag that agrees with LICENSE, a copyright line with
# the holder's name, a Created date that is valid and not in the future, and a
# purpose line.  A shebang may precede it.  Prints each offender and fails.
# deps/ holds the git submodules: their sources are their own projects' to check.
check_source_headers() {
    local root="${1:-.}" today bad=0 total=0 f head_lines line
    today=$(date +%F)
    while IFS= read -r f; do
        total=$((total + 1))
        head_lines=$(head -n 8 "$f")
        line=$(printf '%s\n' "$head_lines" | grep -m1 -E 'SPDX-License-Identifier: ') || line=""
        line="${line##*SPDX-License-Identifier: }"
        if [ "${line%% *}" != "GPL-3.0-only" ]; then
            echo "header: $f: missing or wrong SPDX-License-Identifier (want GPL-3.0-only)" >&2
            bad=$((bad + 1)); continue
        fi
        if ! printf '%s\n' "$head_lines" | grep -q -E 'Copyright \(c\) [0-9]{4} Peter Csaszar \(Császár Péter\)'; then
            echo "header: $f: missing copyright line naming the holder" >&2
            bad=$((bad + 1)); continue
        fi
        line=$(printf '%s\n' "$head_lines" | grep -m1 -E 'Created: [0-9]{4}-[0-9]{2}-[0-9]{2}') || line=""
        created=$(printf '%s\n' "$line" | grep -o -E '[0-9]{4}-[0-9]{2}-[0-9]{2}') || created=""
        if [ -z "$created" ] || [ "$(date -d "$created" +%F 2>&1)" != "$created" ]; then
            echo "header: $f: missing or invalid Created: date" >&2
            bad=$((bad + 1)); continue
        fi
        if [[ "$created" > "$today" ]]; then
            echo "header: $f: Created: $created is in the future" >&2
            bad=$((bad + 1)); continue
        fi
        # The purpose line is the line right after Created:, a non-empty comment.
        if ! printf '%s\n' "$head_lines" | grep -A1 -E 'Created: [0-9]{4}-' | tail -n 1 \
                | grep -q -E '^(/\* |# ).*[A-Za-z]'; then
            echo "header: $f: missing purpose line after Created:" >&2
            bad=$((bad + 1)); continue
        fi
    done < <(find "$root" \( -name .git -o -name 'build*' -o -name bin -o -name tmp \
                              -o -path "$root/deps" \) -prune -o -type f \
                 \( -name '*.c' -o -name '*.h' -o -name '*.sh' -o -name '*.cmake' -o -name CMakeLists.txt \) -print | sort)
    echo "header: $total files checked, $bad without a valid header"
    [ "$bad" -eq 0 ]
}

# The owner's private domain appears in no tracked file (private-names policy).
# The pattern is built from two parts so that this script does not contain the
# name in one piece and fail its own check.  Prints each offending file.
check_private_names() {
    local root="${1:-.}" part_a="csasz" part_b="ar" part_c="\\.em" part_d="ail" found rc=0
    found=$(git -C "$root" grep -I -l -i -E "${part_a}${part_b}${part_c}${part_d}") || rc=$?
    case $rc in
        0) printf '%s\n' "$found" | sed 's/^/names: private domain in /' >&2
           echo "names: $(printf '%s\n' "$found" | wc -l) tracked files name the private domain" >&2
           return 1 ;;
        1) echo "names: no tracked file names the private domain"; return 0 ;;
        *) echo "names: git grep failed in $root" >&2; return 2 ;;
    esac
}

# Every test is declared in tests/CMakeLists.txt and built into
# build/tests/<category>/bin/<name> (testing-model.md): a declared test with no
# executable there, or an executable there that is not declared, is a failure
# of the project.  Usage: check_declared_tests <tests/CMakeLists.txt> <build/tests>
check_declared_tests() {
    local decl="$1" tests_dir="$2" rc=0 cat name line
    local want have
    want=$(sed -n 's/^project_test(\([A-Za-z0-9_-]*\)[[:space:]]\{1,\}\([A-Za-z0-9_.-]*\).*/\1 \2/p' "$decl" | sort -u) || return 2
    if [ -z "$want" ]; then
        echo "declared: no project_test() line in $decl" >&2
        return 1
    fi
    have=""
    for d in "$tests_dir"/*/bin; do
        [ -d "$d" ] || continue
        cat=$(basename "$(dirname "$d")")
        for f in "$d"/*; do
            [ -e "$f" ] || continue
            have="${have}${cat} $(basename "$f")"$'\n'
        done
    done
    have=$(printf '%s' "$have" | sort -u)
    while read -r cat name; do
        [ -n "$cat" ] || continue
        if ! printf '%s\n' "$have" | grep -qx "$cat $name"; then
            echo "declared: $cat/$name is declared but has no executable in $tests_dir/$cat/bin" >&2
            rc=1
        fi
    done <<< "$want"
    while read -r cat name; do
        [ -n "$cat" ] || continue
        if ! printf '%s\n' "$want" | grep -qx "$cat $name"; then
            echo "declared: $tests_dir/$cat/bin/$name is not declared in $decl" >&2
            rc=1
        fi
    done <<< "$have"
    if [ "$rc" -eq 0 ]; then
        echo "declared: $(printf '%s\n' "$want" | wc -l) declared tests, all built, none undeclared"
    fi
    return $rc
}

# The workspace's own libraries come in bundled (git submodules under deps/,
# linked statically) or system (the installed -dev packages); WORKSPACE_LIBS
# selects bundled (the default), system or auto (policies/dependencies-model.md).
# For the bundled way the submodules are fetched here, so nobody types a
# `git submodule` command: a missing or out-of-pin submodule is updated, and a
# pin that is not a release tag (vX.Y.Z) fails the run.
WORKSPACE_LIBS="${WORKSPACE_LIBS:-bundled}"

ensure_workspace_libs() {
    case "$WORKSPACE_LIBS" in
        bundled|system|auto) ;;
        *) echo "ERROR: WORKSPACE_LIBS='$WORKSPACE_LIBS': use bundled, system or auto." >&2
           return 1 ;;
    esac
    [ "$WORKSPACE_LIBS" = system ] && return 0
    local paths path name pin have tag
    if [ ! -f .gitmodules ]; then
        echo "ERROR: no .gitmodules: cannot fetch the bundled libraries;" \
             "install the -dev packages and use WORKSPACE_LIBS=system." >&2
        return 1
    fi
    paths=$(git config -f .gitmodules --get-regexp '^submodule\..*\.path$' | awk '{print $2}') || return 1
    for path in $paths; do
        name=$(basename "$path")
        # auto: an installed package wins, the submodule is not needed.
        if [ "$WORKSPACE_LIBS" = auto ] && pkg-config --exists "$name"; then
            continue
        fi
        pin=$(git ls-files -s -- "$path" | awk '{print $2}')
        have=""
        # a submodule not yet initialised has no .git entry: nothing to ask it
        if [ -e "$path/.git" ]; then
            have=$(git -C "$path" rev-parse HEAD) || have=""
        fi
        if [ -z "$have" ] || [ "$have" != "$pin" ]; then
            echo "Fetching submodule $path (pin ${pin:0:12})..."
            if ! git submodule update --init --recursive -- "$path"; then
                echo "ERROR: cannot fetch $path (no network or no access?)." >&2
                echo "       Fetch it by hand, or use the installed -dev package: WORKSPACE_LIBS=system." >&2
                return 1
            fi
            have=$(git -C "$path" rev-parse HEAD) || return 1
        fi
        tag=$(git -C "$path" tag --points-at "$have" | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -n 1)
        if [ -z "$tag" ]; then
            echo "ERROR: $path is pinned to ${have:0:12}, which is not a release tag (vX.Y.Z)." >&2
            return 1
        fi
        echo "$path: $tag (${have:0:12})"
    done
}

install_deps() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        case "$ID" in
            ubuntu)
                if [[ "$VERSION_ID" == "24.04" ]]; then
                    echo "Detected Ubuntu 24.04. Installing dependencies..."
                    sudo apt-get update
                    sudo apt-get install -y build-essential cmake libssl-dev zlib1g-dev libzstd-dev lcov valgrind dpkg-dev
                else
                    echo "Unsupported Ubuntu version: $VERSION_ID. Only 24.04 is explicitly supported."
                    exit 1
                fi
                ;;
            rocky)
                if [[ "$VERSION_ID" == 9* ]]; then
                    echo "Detected Rocky Linux 9. Installing dependencies..."
                    sudo dnf install -y epel-release
                    sudo dnf groupinstall -y "Development Tools"
                    sudo dnf install -y cmake openssl-devel openssl-static zlib-static libzstd-static glibc-static lcov valgrind rpm-build
                else
                    echo "Unsupported Rocky version: $VERSION_ID. Only 9.x is explicitly supported."
                    exit 1
                fi
                ;;
            *)
                echo "Unsupported OS: $ID. Please install dependencies manually."
                exit 1
                ;;
        esac
    else
        echo "Could not detect OS. Please install dependencies manually."
        exit 1
    fi
}

cmake_configure() {
    local build_type="$1"
    local extra_flags="${2:-}"
    ensure_workspace_libs || exit 1
    mkdir -p "$BUILD_DIR" "$BIN_DIR"
    cd "$BUILD_DIR"
    # Coverage is a per-invocation choice, not a sticky cache entry.  Without an
    # explicit OFF here a previous `coverage` run leaves instrumentation enabled
    # in CMakeCache.txt, and every later build — release binaries and packages
    # included — silently carries gcov instrumentation.  $extra_flags comes
    # last so the coverage target can still turn it back on.
    cmake -DCMAKE_BUILD_TYPE="$build_type" \
          -DENABLE_COVERAGE=OFF \
          -DWORKSPACE_LIBS="$WORKSPACE_LIBS" \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
          $extra_flags ..
    cd ..
    # Symlink compile_commands.json to project root for clangd/LSP
    ln -sf build/compile_commands.json compile_commands.json
}

# All PTY suites, defined once so that `pty` (which must fail the build) and
# `coverage` (which must always produce a report) cannot drift apart.
# Usage: for_each_pty_suite <runner>   where <runner> is called as
#        <runner> <label> <binary> [args…]  from inside the build directory.
for_each_pty_suite() {
    local run="$1"
    local B="$2"   # absolute bin dir
    "$run" views          ./tests/pty/bin/test-pty-views "$B/email-cli" \
                          ./tests/pty/mock-imap-server "$B/email-cli-ro" \
                          "$B/email-sync" "$B/email-tui"
    "$run" gmail-tui      ./tests/pty/bin/test-pty-gmail-tui "$B/email-tui" \
                          "$B/email-sync" ./tests/pty/mock-gmail-server
    "$run" mail-rules     ./tests/pty/bin/test-pty-mail-rules "$B/email-sync" \
                          "$B/email-tui" ./tests/pty/mock-imap-server
    "$run" compose-dialog ./tests/pty/bin/test-pty-compose-dialog "$B/email-tui" \
                          ./tests/pty/mock-imap-server ./tests/pty/mock-smtp-server
    "$run" attachment     ./tests/pty/bin/test-pty-attachment "$B/email-tui" \
                          ./tests/pty/mock-imap-server ./tests/pty/mock-smtp-server
    "$run" send-local     ./tests/pty/bin/test-pty-send-local "$B/email-tui" \
                          "$B/email-sync" ./tests/pty/mock-imap-server \
                          ./tests/pty/mock-smtp-server
    "$run" input-line     ./tests/pty/bin/test-pty-input-line \
                          ./tests/pty/input-line-harness
    "$run" screens        ./tests/pty/bin/test-pty-screens "$B/email-tui" \
                          ./tests/pty/mock-imap-server "$B/email-sync" \
                          ./tests/pty/mock-gmail-server
    "$run" compose-keys   ./tests/pty/bin/test-pty-compose-keys "$B/email-tui" \
                          ./tests/pty/mock-imap-server
    "$run" rules-keys     ./tests/pty/bin/test-pty-rules-keys "$B/email-tui" \
                          ./tests/pty/mock-imap-server
    "$run" help-keys      ./tests/pty/bin/test-pty-help-keys "$B/email-tui" \
                          ./tests/pty/mock-imap-server
    "$run" review-keys    ./tests/pty/bin/test-pty-review-keys "$B/email-tui" \
                          ./tests/pty/mock-imap-server ./tests/pty/mock-smtp-server
    "$run" compose        ./tests/pty/bin/test-pty-compose "$B/email-tui" \
                          ./tests/pty/mock-smtp-server "$B/email-cli"
    # The PTY harness is a git submodule (deps/libptytest) with its own test
    # suite and release gates; they run in its repository, not here.
}

PTY_TARGETS="tests-pty"

JOBS=$(nproc)

cmake_build() {
    cmake --build "$BUILD_DIR" -- -j"$JOBS"
    mkdir -p "$BIN_DIR"
    # A copy that fails must fail the build: otherwise the next suite tests the
    # binary left over from an earlier build (testing-model.md).
    for b in "$PROJECT_NAME" "${PROJECT_NAME}-ro" email-sync email-tui email-import-rules; do
        cp "$BUILD_DIR/$b" "$BIN_DIR/" || exit 1
    done
}

build_release() {
    cmake_configure Release
    cmake_build
    echo "Build complete: $BIN_PATH"
}

build_debug() {
    cmake_configure Debug
    cmake_build
    echo "Debug build (with ASAN) complete: $BIN_PATH"
}

build_test_runner() {
    cmake --build "$BUILD_DIR" --target tests-unit -- -j"$JOBS"
}

do_install() {
    build_release
    mkdir -p "$INSTALL_DIR"
    for bin in $INSTALL_BINS; do
        src="$BIN_DIR/$bin"
        if [ -f "$src" ]; then
            cp "$src" "$INSTALL_DIR/$bin"
            echo "Installed $INSTALL_DIR/$bin"
        else
            echo "Warning: $src not found, skipping."
        fi
    done
    echo "Install complete. Make sure $INSTALL_DIR is in your PATH."
}

do_uninstall() {
    for bin in $INSTALL_BINS; do
        dst="$INSTALL_DIR/$bin"
        if [ -f "$dst" ]; then
            rm "$dst"
            echo "Removed $dst"
        fi
    done
    rm -f -- "$INSTALL_DIR"/libemail-client.so*      # left by installs before EMAIL-103
    echo "Uninstall complete."
}

# Every verb that does work times itself (manage-sh-model.md): the verb is re-run
# by this script under MANAGE_TIMED=1 and one line
# "<UTC time> <verb> <s>s <ok|fail> <commit>" is appended to the timing log,
# whatever the outcome; <commit> is the short id of HEAD, with a "+" after it
# when the working tree had uncommitted changes (EMAIL-121).  MANAGE_TIMING_LOG
# overrides the log path (the tests keep theirs under build/).
TIMING_LOG="${MANAGE_TIMING_LOG:-$BUILD_DIR/timing.log}"
case "$1" in
    build|debug|test|coverage|valgrind|functional|pty|integration|integration-local|e2e|headers|names|declared|check|package|install)
        if [ -z "${MANAGE_TIMED:-}" ]; then
            timed_start=$(date +%s)
            timed_rc=0
            MANAGE_TIMED=1 "$0" "$@" || timed_rc=$?
            timed_end=$(date +%s)
            timed_res=ok
            [ "$timed_rc" -eq 0 ] || timed_res=fail
            mkdir -p "$(dirname "$TIMING_LOG")" || exit 1
            timed_commit=$(git rev-parse --short HEAD) || timed_commit=unknown
            if [ "$timed_commit" != unknown ]; then
                timed_dirty=$(git status --porcelain) || timed_dirty=""
                [ -z "$timed_dirty" ] || timed_commit="${timed_commit}+"
            fi
            printf '%s %s %ss %s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$1" \
                "$((timed_end - timed_start))" "$timed_res" "$timed_commit" >> "$TIMING_LOG" || exit 1
            exit "$timed_rc"
        fi
        ;;
esac

# Prints the gate line from the latest run of each of check, package and install
# in the log.  A part that never ran through this script is left out; with none
# of them the line is "Gate: not measured" and the status is non-zero.
# gate_record: the log lines the gate line is made from (the latest of each
# part), which is what makes two reports the same or different.
gate_record() {
    local verb
    [ -s "$TIMING_LOG" ] || return 0
    for verb in check package install; do
        awk -v v="$verb" '$2 == v { l = $0 } END { if (l != "") print l }' "$TIMING_LOG"
    done
}

gate_line() {
    local verb secs total=0 parts="" n=0
    if [ -s "$TIMING_LOG" ]; then
        for verb in check package install; do
            secs=$(awk -v v="$verb" '$2 == v { s = $3 } END { sub(/s$/, "", s); print s }' "$TIMING_LOG")
            [ -n "$secs" ] || continue
            total=$((total + secs))
            parts="${parts:+$parts, }$verb ${secs}s"
            n=$((n + 1))
        done
    fi
    if [ "$n" -eq 0 ]; then
        echo "Gate: not measured"
        return 1
    fi
    echo "Gate: ${total}s ($parts)"
}

case "$1" in
    deps)
        install_deps
        ensure_workspace_libs
        ;;
    build)
        build_release
        ;;
    debug)
        build_debug
        ;;
    run)
        build_release
        echo "Launching $PROJECT_NAME..."
        $BIN_PATH
        ;;
    test)
        echo "Running unit tests with ASAN..."
        build_debug
        build_test_runner
        # An optional second argument runs only the test suites whose name
        # contains it: ./manage.sh test test_mime
        (cd "$BUILD_DIR" && ./tests/unit/bin/test-runner ${2:+"$2"})
        ;;
    functional)
        echo "Running functional tests..."
        build_release
        cmake --build "$BUILD_DIR" --target tests-functional -- -j"$JOBS"
        functional_rc=0
        "$BUILD_DIR/tests/functional/bin/run_functional" ${2:+--phase "$2"} || functional_rc=$?
        if [ -z "$2" ]; then
            # The other declared functional tests (scripts for the manage.sh
            # verdicts, the package, the static binaries, the skill and the
            # maps ...) run with it: a declared test that nothing runs proves
            # nothing.  Every one is named in the summary.
            functional_summary=""
            for fscript in "$BUILD_DIR"/tests/functional/bin/*; do
                fname=$(basename "$fscript")
                [ "$fname" = run_functional ] && continue
                echo "=== functional: $fname ==="
                if MANAGE_TIMED= "$fscript"; then
                    functional_summary="${functional_summary}  $(printf '%-26s' "$fname") PASS"$'\n'
                else
                    functional_summary="${functional_summary}  $(printf '%-26s' "$fname") FAIL"$'\n'
                    functional_rc=1
                fi
            done
            echo ""
            echo "=== functional: scripts summary ==="
            printf '%s' "$functional_summary"
        fi
        [ "$functional_rc" -eq 0 ] || { echo "functional tests FAILED" >&2; exit 1; }
        ;;
    pty)
        echo "Running PTY tests..."
        build_release
        cmake --build "$BUILD_DIR" --target $PTY_TARGETS -- -j"$JOBS"
        ABS_BUILD="$(realpath "$BUILD_DIR")"
        ABS_BIN="$(realpath "$BIN_DIR")"
        # ./manage.sh pty [SUITE [FILTER]]: SUITE is a label from
        # for_each_pty_suite, FILTER is passed on as TEST_FILTER and selects
        # the tests of that suite whose function name contains it.
        pty_suite="$2"
        if [ -n "$3" ]; then export TEST_FILTER="$3"; fi
        pty_rc=0
        pty_ran=0
        run_pty_strict() {  # run_pty_strict <label> <binary> [args…]
            local label="$1"; shift
            if [ -n "$pty_suite" ] && [ "$pty_suite" != "$label" ]; then
                return 0
            fi
            pty_ran=$((pty_ran + 1))
            echo "--- PTY: $label ---"
            # Every mock binds a port the kernel picks and every fixture lives
            # under build/ with the pid in its name, so suites share nothing.
            if ! (cd "$ABS_BUILD" && "$@"); then
                echo "  [FAILED] $label"
                pty_rc=1
            fi
        }
        for_each_pty_suite run_pty_strict "$ABS_BIN"
        if [ -n "$pty_suite" ] && [ "$pty_ran" -eq 0 ]; then
            echo "ERROR: no PTY suite is called '$pty_suite'" >&2
            exit 1
        fi
        exit $pty_rc
        ;;
    valgrind)
        echo "Running unit tests with Valgrind..."
        build_release
        build_test_runner
        (cd "$BUILD_DIR" && valgrind --leak-check=full --error-exitcode=1 \
            --child-silent-after-fork=yes ./tests/unit/bin/test-runner)
        ;;
    coverage)
        cmake_configure Debug "-DENABLE_COVERAGE=ON"
        cmake_build
        build_test_runner
        echo "Building PTY test binaries..."
        cmake --build "$BUILD_DIR" --target $PTY_TARGETS tests-functional -- -j"$JOBS"

        # Pass 1 — functional suite + PTY tests (fresh .gcda) → functional badge
        find "$BUILD_DIR" -name "*.gcda" -delete
        # The report is still produced when the suite fails, but the failure is
        # the verdict of this run: it is kept and returned at the end.
        functional_rc=0
        "$BUILD_DIR/tests/functional/bin/run_functional" || functional_rc=$?
        pty_cov_rc=0
        echo "Running PTY tests for coverage..."
        ABS_BUILD="$(realpath "$BUILD_DIR")"
        ABS_BIN="$(realpath "$BIN_DIR")"
        # Every suite contributes to the measured coverage.  Failures are
        # tolerated here on purpose — the report must still be produced — but
        # they are reported, and `./manage.sh pty` (which CI runs) fails on them.
        run_pty_lenient() {  # run_pty_lenient <label> <binary> [args…]
            local label="$1"; shift
            # PTY tests run from the build directory so that mock servers find
            # tests/certs/test.crt relative to cwd.  Output is captured rather
            # than discarded: a suite that fails only under the coverage build
            # is exactly the one whose failure lines nobody can reconstruct
            # afterwards, and a bare "reported failures" says nothing.
            local log="$ABS_BUILD/pty-coverage-$label.log"
            if ! (cd "$ABS_BUILD" && "$@" >"$log" 2>&1); then
                echo "  [warn] PTY suite '$label' reported failures (coverage run continues):"
                grep -E "\[FAIL\]|ASSERT|Segmentation|Assertion" "$log" | head -20 | sed 's/^/    /'
                echo "    (full output: $log)"
                pty_cov_rc=1
            fi
        }
        # A program that is killed while it writes its .gcda at exit leaves a
        # truncated file that fails lcov (EMAIL-119): the PTY harness lets a
        # child that is on its way out finish before it signals it.
        export PTYTEST_CLOSE_GRACE_MS="${PTYTEST_CLOSE_GRACE_MS:-3000}"
        for_each_pty_suite run_pty_lenient "$ABS_BIN"
        gcda_rc=0
        tests/functional/gcda_check.sh "$ABS_BUILD" || gcda_rc=$?
        # One definition of "product code" for every report: the program's own
        # sources.  Test code, the PTY library and the system headers are not
        # measured (policies/testing-model.md, Coverage).
        ROOT_ABS="$(pwd)"
        capture_product() {  # capture_product <raw.info> <product.info>
            (cd "$BUILD_DIR" && lcov --capture --directory . --output-file "$1" && \
             lcov --extract "$1" --ignore-errors unused \
                  "$ROOT_ABS/src/*" "$ROOT_ABS/libemail/src/*" "$ROOT_ABS/libemail-client/src/*" \
                  --output-file "$2")
        }
        line_pct() {         # line_pct <product.info> -> "91.8"
            lcov --summary "$BUILD_DIR/$1" 2>&1 | grep 'lines' | grep -oP '[0-9]+\.[0-9]+(?=%)' | head -1
        }

        # External suites alone: functional + PTY (black-box, instrumented binaries).
        echo "Capturing external (functional + PTY) coverage..."
        capture_product coverage-functional-raw.info coverage-functional.info

        # Unit tests on top of the same .gcda give the union of everything.
        # (LCOV 2.x --add-tracefile intersects lines instead of unioning; running
        # both kinds in sequence and capturing once gives the correct union.)
        (cd "$BUILD_DIR" && ./tests/unit/bin/test-runner)
        echo "Capturing combined coverage..."
        capture_product coverage-raw.info coverage.info

        # Unit tests alone, from fresh counters: they have to stand on their own.
        find "$BUILD_DIR" -name "*.gcda" -delete
        (cd "$BUILD_DIR" && ./tests/unit/bin/test-runner)
        echo "Capturing unit-only coverage..."
        capture_product coverage-unit-raw.info coverage-unit.info

        echo "Generating coverage reports..."
        (cd "$BUILD_DIR" && \
         genhtml coverage.info --output-directory coverage_report && \
         genhtml coverage-functional.info --output-directory coverage_functional_report)

        UNIT_PCT=$(line_pct coverage-unit.info)
        EXT_PCT=$(line_pct coverage-functional.info)
        ALL_PCT=$(line_pct coverage.info)
        COVERAGE_MIN=80      # policy floor for unit alone and for the external suites
        COVERAGE_TARGET=90   # policy target for everything together (not a gate)
        echo ""
        echo "Line coverage of product code (src/, libemail/src/, libemail-client/src/):"
        echo "  unit tests alone            ${UNIT_PCT}%   (minimum ${COVERAGE_MIN}%)"
        echo "  functional + PTY together   ${EXT_PCT}%   (minimum ${COVERAGE_MIN}%)"
        echo "  everything combined         ${ALL_PCT}%   (target ${COVERAGE_TARGET}%)"
        # Kept for the per-release record (docs/dev/testing.md), so the figures
        # quoted anywhere are the ones of a run, with its date and commit.
        tests/functional/coverage_summary.sh "$(date +%F)" "$(git rev-parse --short=12 HEAD)" \
            "$BUILD_DIR/coverage-unit.info" "$BUILD_DIR/coverage-functional.info" \
            "$BUILD_DIR/coverage.info" | tee "$BUILD_DIR/coverage-summary.txt"
        echo "Combined report:  $BUILD_DIR/coverage_report/index.html"
        echo "External report:  $BUILD_DIR/coverage_functional_report/index.html"
        coverage_gate_rc=0
        for pair in "unit tests alone:$UNIT_PCT" "functional + PTY:$EXT_PCT"; do
            label="${pair%%:*}"; pct="${pair##*:}"
            if [ -z "$pct" ]; then
                echo "ERROR: no coverage figure for '$label'." >&2
                coverage_gate_rc=1
            elif awk -v p="$pct" -v m="$COVERAGE_MIN" 'BEGIN{exit !(p+0 < m+0)}'; then
                echo "ERROR: coverage of '$label' is ${pct}%, below the ${COVERAGE_MIN}% minimum." >&2
                coverage_gate_rc=1
            fi
        done
        if awk -v p="$ALL_PCT" -v m="$COVERAGE_TARGET" 'BEGIN{exit !(p+0 < m+0)}'; then
            echo "note: the combined figure is below the ${COVERAGE_TARGET}% target (not a gate)."
        fi
        if [ "$gcda_rc" -ne 0 ]; then
            echo "ERROR: coverage run failed: invalid .gcda files (named above, see build/pty-coverage-*.log)." >&2
            exit 1
        fi
        if [ "$functional_rc" -ne 0 ] || [ "$pty_cov_rc" -ne 0 ]; then
            echo "ERROR: coverage run failed: functional suite status $functional_rc," \
                 "PTY suites status $pty_cov_rc (reports above were still produced)." >&2
            exit 1
        fi
        if [ "$coverage_gate_rc" -ne 0 ]; then
            exit 1
        fi
        ;;
    integration)
        build_release
        ./tests/integration/run_integration.sh
        ;;
    integration-local)
        build_release
        ./tests/integration/run_local_dovecot.sh
        ;;
    e2e)
        # End-to-end tests (tests/e2e): they install the package into an LXC
        # container they create themselves, so the package is built first.
        ./manage.sh package deb || exit 1
        cmake --build "$BUILD_DIR" --target tests-e2e -- -j"$JOBS" || exit 1
        e2e_rc=0
        e2e_ran=0
        for e2e_test in "$BUILD_DIR"/tests/e2e/bin/*; do
            [ -x "$e2e_test" ] || continue
            if [ -n "${2:-}" ] && [ "$(basename "$e2e_test")" != "$2" ]; then continue; fi
            e2e_ran=$((e2e_ran + 1))
            echo "--- e2e: $(basename "$e2e_test") ---"
            "$e2e_test" || e2e_rc=1
        done
        if [ "$e2e_ran" -eq 0 ]; then
            echo "ERROR: no e2e test ran${2:+ (no test named $2)}" >&2
            exit 1
        fi
        exit "$e2e_rc"
        ;;
    imap-down)
        ./tests/integration/run_integration.sh --down
        ;;
    imap-clean)
        ./tests/integration/run_integration.sh --clean
        ;;
    headers)
        check_source_headers "${2:-.}"
        ;;
    names)
        check_private_names "${2:-.}"
        ;;
    declared)
        # ./manage.sh declared [TESTS_CMAKELISTS [BUILD_TESTS_DIR]]
        if [ $# -lt 2 ]; then
            build_release
            cmake --build "$BUILD_DIR" --target tests -- -j"$JOBS"
        fi
        check_declared_tests "${2:-tests/CMakeLists.txt}" "${3:-$BUILD_DIR/tests}"
        ;;
    check)
        # Every gate, each in its own run, one summary to compare with CI.
        # Coverage joins when it has a threshold that fails (EMAIL-15).
        check_rc=0
        check_summary=""
        for gate in headers names declared test functional pty valgrind; do
            echo "=== check: $gate ==="
            # MANAGE_TIMED is cleared so every gate logs its own time.
            if MANAGE_TIMED= "$0" "$gate"; then
                check_summary="${check_summary}  $(printf '%-12s' "$gate") PASS"$'\n'
            else
                check_summary="${check_summary}  $(printf '%-12s' "$gate") FAIL"$'\n'
                check_rc=1
            fi
        done
        echo ""
        echo "=== check: summary ==="
        printf '%s' "$check_summary"
        if [ "$check_rc" -ne 0 ]; then
            echo "check FAILED" >&2
        else
            echo "check passed"
        fi
        exit $check_rc
        ;;
    install)
        do_install
        ;;
    package)
        build_release
        target="${2:-all}"
        case "$target" in
            deb)  generators="DEB" ;;
            rpm)  generators="RPM" ;;
            all)  generators="DEB;RPM" ;;
            *)
                echo "Unknown package target: $target  (use: deb | rpm | all)"
                exit 1
                ;;
        esac
        echo "Creating package(s): $generators"
        # build/packages/ holds exactly the packages of this run: it is emptied
        # first, and left empty when the run fails (packaging policy, "Where
        # the output goes").  The cd is checked so the rm can only act there.
        PKG_DIR="$(cd "$BUILD_DIR" && pwd)/packages"
        mkdir -p "$PKG_DIR" || exit 1
        ( cd "$PKG_DIR" && rm -f -- ./*.deb ./*.rpm && rm -rf _CPack_Packages ) || exit 1
        # umask 022: CPack copies the caller's umask into the directory modes
        # of the package (packaging policy, "The umask leak").
        if ! ( umask 022; cd "$BUILD_DIR" && cpack -G "$generators" --config CPackConfig.cmake -B "$PKG_DIR" ); then
            ( cd "$PKG_DIR" && rm -f -- ./*.deb ./*.rpm && rm -rf _CPack_Packages )
            echo "ERROR: packaging failed; $PKG_DIR is left empty." >&2
            exit 1
        fi
        # CPack's staging tree is not a package; the directory is the packages.
        ( cd "$PKG_DIR" && rm -rf _CPack_Packages ) || exit 1
        # One package per requested generator, or the run is not a result.
        built=$(find "$PKG_DIR" -maxdepth 1 -type f \( -name '*.deb' -o -name '*.rpm' \) | wc -l)
        if [ "$built" -lt 1 ]; then
            echo "ERROR: cpack reported success but $PKG_DIR holds no package." >&2
            exit 1
        fi
        # A package that is not lintian-clean is not a result (packaging policy,
        # "The package passes lintian"): the packages are removed and the run fails.
        for deb in "$PKG_DIR"/*.deb; do
            [ -e "$deb" ] || continue
            if ! command -v lintian >/dev/null; then
                echo "ERROR: lintian is not installed; cannot check $deb." >&2
                ( cd "$PKG_DIR" && rm -f -- ./*.deb ./*.rpm )
                exit 1
            fi
            if ! lintian --fail-on error,warning "$deb"; then
                echo "ERROR: lintian findings in $deb; $PKG_DIR is left empty." >&2
                ( cd "$PKG_DIR" && rm -f -- ./*.deb ./*.rpm )
                exit 1
            fi
        done
        echo "Package(s) written to $PKG_DIR/"
        ls -lh "$PKG_DIR"
        ;;
    uninstall)
        do_uninstall
        ;;
    timing)
        line=$(gate_line) || { echo "$line"; exit 1; }
        echo "$line"
        if [ -n "$2" ]; then
            # A measurement is posted once: build/timing.posted keeps, per key,
            # the record (the log lines used) last posted.  The same record is
            # not posted again; a record from a newer run is, even when the
            # text of the line is identical.
            posted="$(dirname "$TIMING_LOG")/timing.posted"
            record_sum=$(gate_record | cksum | cut -d' ' -f1,2 | tr ' ' '-')
            if [ -f "$posted" ] && grep -qx "$2 $record_sum" "$posted"; then
                echo "timing: this measurement is already posted on $2; nothing posted."
                exit 0
            fi
            issue-cli issue comment "$2" "$line"
            { [ ! -f "$posted" ] || grep -v "^$2 " "$posted" || [ "$?" -eq 1 ]; echo "$2 $record_sum"; } > "$posted.new" \
                && mv "$posted.new" "$posted"
        fi
        ;;
    clean-logs)
        if [ -f "$BIN_PATH" ]; then
            $BIN_PATH --clean-logs
        else
            echo "Binary not found. Attempting manual cleanup..."
            rm -rf ~/.cache/email-cli/logs/*
            echo "Logs cleaned."
        fi
        ;;
    clean)
        rm -rf "./build" "./bin"
        echo "Cleaned."
        ;;
    help|*)
        show_help
        ;;
esac
