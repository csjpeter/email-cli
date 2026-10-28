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
    echo "  deps           Install system dependencies (supports Ubuntu 24.04, Rocky 9); includes test-base"
    echo "  test-base      Create or refresh the test base containers in /var/lib/lxc-bases (email-cli has none)"
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
    echo "  valgrind       Build and run every unit suite under Valgrind"
    echo "  sanitize       Debug build of every program and test with ASan and UBSan; run the unit,"
    echo "                 functional, pty and integration suites on those binaries;"
    echo "                 logs in build/sanitize/, any log is a failure"
    echo "  memcheck       Dynamic Release build; run the same suites with every program under"
    echo "                 Valgrind (leak-check=full, track-origins); logs in build/valgrind/"
    echo "  headers [DIR]  Fail on a source file or script without the source header"
    echo "  names [DIR]    Fail when a tracked file names the owner's private domain"
    echo "  declared       Fail when a declared test is not built or a built test is not declared"
    echo "  check          Run every gate (headers, names, declared, test, functional, pty, valgrind) and summarise"
    echo "  coverage       Run the unit scope and the external scope (functional + pty +"
    echo "                 integration), report line coverage, keep the raw data in"
    echo "                 build/coverage/unit and build/coverage/external"
    echo "  integration    Run tests/integration against a per-agent Dovecot process"
    echo "  install        Build (release) and install binaries to ~/.local/bin"
    echo "  uninstall      Remove installed binaries from ~/.local/bin"
    echo "  package [deb|rpm|all]  Build release and create package(s) in build/packages/"
    echo "  tag-build VERSION  Annotated tag vVERSION on HEAD (ticket keys since the previous tag); local only, pushes nothing"
    echo "  timing [KEY-N] Print the Gate line from build/timing.log; with a key, comment it on the ticket"
    echo "  clean-logs     Purge all application log files"
    echo "  clean          Remove all build artifacts"
    echo "  help           Show this help message"
    echo ""
    echo "test, functional, pty, integration, valgrind and coverage write build/result/VERB.result"
    echo "(policies/manage-sh-model.md); an unknown command prints this text to stderr and exits 2."
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

# test_base: creates or refreshes the project's test base containers in
# /var/lib/lxc-bases, idempotently (agent-container-model.md, section 4a).
# email-cli has none: its e2e test makes a throwaway container per run
# (tests/common/testenv.sh), so there is nothing to create or refresh.
test_base() {
    echo "test-base: email-cli has no test base container; nothing to create or refresh."
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
          -DENABLE_UBSAN=OFF \
          -DEMAIL_STATIC=ON \
          -DCMAKE_C_FLAGS= \
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
    "$run" history        ./tests/pty/bin/test-pty-history "$B/email-tui" \
                          ./tests/pty/mock-imap-server "$B/email-cli"
    "$run" compose        ./tests/pty/bin/test-pty-compose "$B/email-tui" \
                          ./tests/pty/mock-smtp-server "$B/email-cli"
    # The PTY harness is a git submodule (deps/libptytest) with its own test
    # suite and release gates; they run in its repository, not here.
}

# sanitize and memcheck (EMAIL-224) run the suites with the programs under
# test instrumented.  The suites find the programs through EMAIL_TEST_BIN_DIR
# (default: bin/) and, for the mock servers they compile themselves,
# EMAIL_TEST_MOCK_CFLAGS.
#
# instrumented_run LABEL RUNNER_ENV... is not used; the verbs below call:
#   run_instrumented_suites ABS_BIN LABEL_ENV_VAR
# which runs unit, functional, pty and integration and returns 0 when all
# of them passed.  LABEL_ENV_VAR is the name of the variable that tells the
# instrumented program which suite started it (VG_TEST_NAME for Valgrind, none
# needed for the sanitizers, whose log names carry the pid).
run_instrumented_suites() {
    local abs_bin="$1" label_var="${2:-}" rc=0
    local abs_build
    abs_build="$(realpath "$BUILD_DIR")"
    if [ -z "${SKIP_UNIT:-}" ]; then
        echo "--- instrumented: unit tests ---"
        (cd "$BUILD_DIR" && ./tests/unit/bin/test-runner) || { echo "  [FAILED] unit"; rc=1; }
    fi
    echo "--- instrumented: functional tests ---"
    [ -z "$label_var" ] || export "$label_var=functional"
    "$BUILD_DIR/tests/functional/bin/run_functional" || { echo "  [FAILED] functional"; rc=1; }
    run_pty_instrumented() {  # <label> <binary> [args...]
        local label="$1"; shift
        echo "--- instrumented: PTY $label ---"
        [ -z "$label_var" ] || export "$label_var=pty-$label"
        (cd "$abs_build" && "$@") || { echo "  [FAILED] pty $label"; rc=1; }
    }
    for_each_pty_suite run_pty_instrumented "$abs_bin"
    # No skip: a machine without Dovecot fails the integration scripts (their
    # own tool check), and so this verb.
    echo "--- instrumented: integration ---"
    [ -z "$label_var" ] || export "$label_var=integration-local"
    ./tests/integration/run_local_dovecot.sh || { echo "  [FAILED] integration (run_local_dovecot)"; rc=1; }
    [ -z "$label_var" ] || export "$label_var=integration"
    ./tests/integration/run_integration.sh || { echo "  [FAILED] integration (run_integration)"; rc=1; }
    return $rc
}

# Empty a log directory of a previous run: its files only, never a recursive delete.
fresh_log_dir() {
    mkdir -p "$1"
    find "$1" -maxdepth 1 -type f -delete
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

# Tag the built commit v<VERSION> (annotated; the message names the ticket keys
# since the previous v* tag), locally.  It never pushes (EMAIL-241: the agent
# pushes with gh-push on the host).  The same tag at the same commit again
# is fine; the same tag at another commit is an error.  (EMAIL-229)
tag_package_build() {
    local ver="$1" tag="v$1" head at prev range keys
    head=$(git rev-parse HEAD) || return 1
    if git rev-parse -q --verify "refs/tags/$tag" > /dev/null; then
        at=$(git rev-parse "$tag^{commit}") || return 1
        if [ "$at" != "$head" ]; then
            echo "ERROR: tag $tag exists at ${at:0:12}, not at HEAD ${head:0:12}." >&2
            return 1
        fi
    else
        prev=$(git tag --merged HEAD --list 'v[0-9]*' --sort=-creatordate | head -n 1)
        if [ -n "$prev" ]; then range="$prev..HEAD"; else range="HEAD"; fi
        keys=$(git log --format=%B "$range" | grep -oE '[A-Z][A-Z0-9]*-[0-9]+' | sort -u | tr '\n' ' ')
        git tag -a "$tag" -m "email-cli $ver: ${keys:-no ticket keys}" || return 1
    fi
    echo "Tagged $tag (not pushed)"
}

# The commit the code of a run is at: the short id of HEAD, with a "+" after it
# when the working tree has uncommitted changes (EMAIL-121); "unknown" outside a
# repository.  Used by the timing record and by the result files.
short_commit() {
    local c d
    c=$(git rev-parse --short HEAD) || { echo unknown; return 0; }
    d=$(git status --porcelain) || d=""
    [ -z "$d" ] || c="${c}+"
    echo "$c"
}

# The result file of a verb (manage-sh-model.md, "The result file", EMAIL-233).
# result_begin VERB deletes build/result/VERB.result, writes it again as
# "status: running" and arms an EXIT trap that writes the final file on every
# way out of the verb: success, a failed suite, a failed build, a signal.
# run_suite SUITE DIR CMD... runs one suite with its output shown and kept in
# build/result/log/, counts what the suite's runner counted and records one
# "suite:" line.  The verb exits with the failure of any suite after all ran.
RESULT_DIR="$BUILD_DIR/result"
R_VERB=""; R_FILE=""; R_START=0; R_STARTED=""; R_BAD=0; R_PREFIX=""
R_SUITES=(); R_EXTRA=()

result_write() {  # result_write STATUS EXITCODE
    local status="$1" code="$2" tmp="$R_FILE.new" a=0 f=0 line sa sf
    for line in "${R_SUITES[@]}"; do
        read -r _ _ sa sf _ <<< "$line"
        a=$((a + sa)); f=$((f + sf))
    done
    {
        echo "format: manage-result/1"
        echo "verb: $R_VERB"
        echo "commit: $(short_commit)"
        echo "started: $R_STARTED"
        echo "seconds: $(( $(date +%s) - R_START ))"
        echo "exit: $code"
        echo "status: $status"
        echo "suites: ${#R_SUITES[@]}"
        echo "assertions: $a"
        echo "failed: $f"
        for line in "${R_EXTRA[@]}"; do echo "$line"; done
        for line in "${R_SUITES[@]}"; do echo "suite: $line"; done
    } > "$tmp" && mv -- "$tmp" "$R_FILE"
}

result_exit() {
    local rc=$? status
    trap - EXIT
    if [ "$rc" -eq 0 ] && [ "$R_BAD" -gt 0 ]; then rc=1; fi
    if [ "$rc" -eq 0 ] && [ "${#R_SUITES[@]}" -lt 1 ]; then
        echo "ERROR: $R_VERB ran no suite." >&2
        rc=1
    fi
    if [ "$rc" -eq 0 ]; then status=pass
    elif [ "$R_BAD" -gt 0 ]; then status=fail
    else status=error; fi
    result_write "$status" "$rc" || { echo "ERROR: cannot write $R_FILE" >&2; rc=1; }
    exit "$rc"
}

result_begin() {  # result_begin VERB
    R_VERB="$1"
    mkdir -p "$RESULT_DIR/log" || exit 1
    R_FILE="$(cd "$RESULT_DIR" && pwd)/$1.result"
    rm -f -- "$R_FILE"
    R_START=$(date +%s); R_STARTED=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    R_BAD=0; R_PREFIX=""; R_SUITES=(); R_EXTRA=()
    trap 'exit 143' TERM
    trap 'exit 130' INT
    trap 'exit 129' HUP
    trap result_exit EXIT
    result_write running 0 || exit 1
}

# count_assertions LOG prints "RUN FAILED" as the suite's own runner counted:
# "Tests Run:/Tests Failed:" (unit and pty runners), else "Passed: P / T"
# (functional and integration scripts), else the [PASS] and [FAIL] lines.
count_assertions() {
    awk '
        /^Tests Run:/    { r += $3; have_r = 1 }
        /^Tests Failed:/ { rf += $3 }
        /^Passed: [0-9]+ \/ [0-9]+/ { t += $4; tf += $4 - $2; have_t = 1 }
        /^[ \t]*\[PASS\]/ { pc++ }
        /^[ \t]*\[FAIL\]/ { fc++ }
        END {
            if (have_r)      print r + 0, rf + 0
            else if (have_t) print t + 0, tf + 0
            else             print pc + fc, fc + 0
        }' "$1"
}

run_suite() {  # run_suite NAME DIR CMD [ARGS...]
    local name="$1" dir="$2" log t0 t1 rc a f st
    shift 2
    log="$RESULT_DIR/log/$R_VERB-${R_PREFIX//\//-}$name.log"
    t0=$(date +%s)
    echo "--- $R_VERB: $R_PREFIX$name ---"
    ( cd "$dir" && "$@" ) 2>&1 | tee "$log"
    rc=${PIPESTATUS[0]}
    t1=$(date +%s)
    read -r a f < <(count_assertions "$log")
    if [ "$rc" -eq 0 ] && [ "$a" -ge 1 ] && [ "$f" -eq 0 ]; then st=pass
    elif [ "$rc" -eq 124 ]; then st=timeout
    elif [ "$rc" -ne 0 ] && [ "$a" -eq 0 ]; then st=crash
    else st=fail; fi
    R_SUITES+=("$R_PREFIX$name $st $a $f $((t1 - t0))")
    result_write running 0 || exit 1
    if [ "$st" != pass ]; then
        R_BAD=$((R_BAD + 1))
        echo "  [FAILED] $R_PREFIX$name: $st (exit $rc, $a assertions counted, $f failed)" >&2
        return 1
    fi
}

# The suites of the verbs, defined once so that a verb and the scope of
# `coverage` named like it run the same ones.
run_functional_suites() {  # run_functional_suites [PHASES] [WITH_SCRIPTS: 1|0]
    local phases="${1:-}" with_scripts="${2:-1}" rc=0 fscript fname
    cmake --build "$BUILD_DIR" --target tests-functional -- -j"$JOBS" || return 1
    run_suite run_functional . "$BUILD_DIR/tests/functional/bin/run_functional" ${phases:+--phase "$phases"} || rc=1
    if [ -z "$phases" ] && [ "$with_scripts" = 1 ]; then
        # The other declared functional tests (scripts for the manage.sh
        # verdicts, the package, the static binaries, the skill and the
        # maps ...) run with it: a declared test that nothing runs proves
        # nothing.  Every one is a suite of its own in the result file.
        for fscript in "$BUILD_DIR"/tests/functional/bin/*; do
            fname=$(basename "$fscript")
            [ "$fname" = run_functional ] && continue
            run_suite "$fname" . env MANAGE_TIMED= "$fscript" || rc=1
        done
    fi
    return $rc
}

run_pty_suites() {  # run_pty_suites [SUITE] : all PTY suites, or the one labelled SUITE
    local pty_suite="${1:-}" abs_build abs_bin
    abs_build="$(realpath "$BUILD_DIR")"
    abs_bin="$(realpath "$BIN_DIR")"
    pty_rc=0
    pty_ran=0
    # Every mock binds a port the kernel picks and every fixture lives under
    # build/ with the pid in its name, so suites share nothing; a suite runs
    # from the build directory so that the mocks find tests/certs/test.crt.
    run_pty_one() {  # run_pty_one <label> <binary> [args…]
        local label="$1"; shift
        if [ -n "$pty_suite" ] && [ "$pty_suite" != "$label" ]; then
            return 0
        fi
        pty_ran=$((pty_ran + 1))
        run_suite "$label" "$abs_build" "$@" || pty_rc=1
    }
    for_each_pty_suite run_pty_one "$abs_bin"
    if [ -n "$pty_suite" ] && [ "$pty_ran" -eq 0 ]; then
        echo "ERROR: no PTY suite is called '$pty_suite'" >&2
        return 1
    fi
    return $pty_rc
}

run_integration_suites() {
    local rc=0
    run_suite run_integration . ./tests/integration/run_integration.sh || rc=1
    run_suite run_local_dovecot . ./tests/integration/run_local_dovecot.sh || rc=1
    return $rc
}

# Every verb that does work times itself (manage-sh-model.md): the verb is re-run
# by this script under MANAGE_TIMED=1 and one line
# "<UTC time> <verb> <s>s <ok|fail> <commit>" is appended to the timing log,
# whatever the outcome; <commit> is the short id of HEAD, with a "+" after it
# when the working tree had uncommitted changes (EMAIL-121).  MANAGE_TIMING_LOG
# overrides the log path (the tests keep theirs under build/).
TIMING_LOG="${MANAGE_TIMING_LOG:-$BUILD_DIR/timing.log}"
case "$1" in
    build|debug|test|coverage|valgrind|functional|pty|integration|headers|names|declared|check|package|install)
        if [ -z "${MANAGE_TIMED:-}" ]; then
            timed_start=$(date +%s)
            timed_rc=0
            MANAGE_TIMED=1 "$0" "$@" || timed_rc=$?
            timed_end=$(date +%s)
            timed_res=ok
            [ "$timed_rc" -eq 0 ] || timed_res=fail
            mkdir -p "$(dirname "$TIMING_LOG")" || exit 1
            timed_commit=$(short_commit)
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
        test_base
        ;;
    test-base)
        test_base
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
        result_begin test
        echo "Running unit tests with ASAN..."
        build_debug
        build_test_runner
        # An optional second argument runs only the test suites whose name
        # contains it: ./manage.sh test test_mime
        run_suite unit "$BUILD_DIR" ./tests/unit/bin/test-runner ${2:+"$2"} || exit 1
        ;;
    functional)
        result_begin functional
        echo "Running functional tests..."
        build_release
        run_functional_suites "${2:-}" 1 || { echo "functional tests FAILED" >&2; exit 1; }
        ;;
    pty)
        result_begin pty
        echo "Running PTY tests..."
        build_release
        cmake --build "$BUILD_DIR" --target $PTY_TARGETS -- -j"$JOBS"
        # ./manage.sh pty [SUITE [FILTER]]: SUITE is a label from
        # for_each_pty_suite, FILTER is passed on as TEST_FILTER and selects
        # the tests of that suite whose function name contains it.
        if [ -n "${3:-}" ]; then export TEST_FILTER="$3"; fi
        run_pty_suites "${2:-}" || exit 1
        ;;
    valgrind)
        result_begin valgrind
        echo "Running unit tests with Valgrind..."
        build_release
        build_test_runner
        run_suite unit "$BUILD_DIR" valgrind --leak-check=full --error-exitcode=1 \
            --child-silent-after-fork=yes ./tests/unit/bin/test-runner || exit 1
        ;;
    sanitize)
        echo "Running unit, functional, pty and integration with ASan and UBSan..."
        cmake_configure Debug "-DENABLE_UBSAN=ON"
        cmake_build
        cmake --build "$BUILD_DIR" --target tests-unit tests-functional $PTY_TARGETS -- -j"$JOBS"
        SAN_DIR="$(realpath "$BUILD_DIR")/sanitize"
        fresh_log_dir "$SAN_DIR"
        # Every process of the run, the programs and the mock servers, writes a
        # log of its own (…/asan.PID, …/ubsan.PID) only when it has a finding.
        export ASAN_OPTIONS="log_path=$SAN_DIR/asan:detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1"
        export UBSAN_OPTIONS="log_path=$SAN_DIR/ubsan:halt_on_error=1:print_stacktrace=1"
        export EMAIL_TEST_BIN_DIR="$(realpath "$BIN_DIR")"
        export EMAIL_TEST_MOCK_CFLAGS="-g -fsanitize=address,undefined -fno-omit-frame-pointer"
        san_rc=0
        run_instrumented_suites "$EMAIL_TEST_BIN_DIR" "" || san_rc=1
        ./tests/sanitize/scan_logs.sh sanitizer "$SAN_DIR" || san_rc=1
        [ "$san_rc" -eq 0 ] || { echo "sanitize FAILED (logs: $SAN_DIR)" >&2; exit 1; }
        echo "sanitize passed"
        ;;
    memcheck)
        echo "Running unit, functional, pty and integration under Valgrind..."
        cmake_configure Release "-DEMAIL_STATIC=OFF -DCMAKE_C_FLAGS=-g"
        cmake_build
        cmake --build "$BUILD_DIR" --target tests-unit tests-functional $PTY_TARGETS -- -j"$JOBS"
        ABS_BUILD="$(realpath "$BUILD_DIR")"
        VG_BIN="$ABS_BUILD/valgrind-bin"
        export VG_LOG_DIR="$ABS_BUILD/valgrind"
        fresh_log_dir "$VG_BIN"
        fresh_log_dir "$VG_LOG_DIR"
        export VG_SUPPRESSIONS="$(realpath tests/sanitize/valgrind.supp)"
        # Each program sits in VG_BIN as NAME.bin with the wrapper script as NAME:
        # whatever starts NAME (a PTY child, the daemon the TUI forks, found in
        # the directory of the running program) gets Valgrind with it.
        for b in "$PROJECT_NAME" "${PROJECT_NAME}-ro" email-sync email-tui email-import-rules; do
            cp "$BIN_DIR/$b" "$VG_BIN/$b.bin" || exit 1
            cp tests/sanitize/valgrind-program.sh "$VG_BIN/$b" || exit 1
        done
        export EMAIL_TEST_BIN_DIR="$VG_BIN"
        # memcheck is ten to fifty times slower: the PTY tests stretch every wait
        # for a screen (tests/pty/wait_scale.h) by this factor.
        export EMAIL_TEST_WAIT_SCALE=6
        # The unit tests run under Valgrind as one process, as in `valgrind`.
        vg_rc=0
        echo "--- valgrind: unit tests ---"
        (cd "$BUILD_DIR" && valgrind --leak-check=full --error-exitcode=1 --track-origins=yes \
            --child-silent-after-fork=yes --suppressions="$VG_SUPPRESSIONS" \
            --log-file="$VG_LOG_DIR/unit-test-runner.%p.log" ./tests/unit/bin/test-runner) \
            || { echo "  [FAILED] unit"; vg_rc=1; }
        # run_instrumented_suites runs the unit tests itself; for Valgrind they
        # were run above, so only the other suites go through it.
        SKIP_UNIT=1 run_instrumented_suites "$VG_BIN" VG_TEST_NAME || vg_rc=1
        ./tests/sanitize/scan_logs.sh valgrind "$VG_LOG_DIR" || vg_rc=1
        [ "$vg_rc" -eq 0 ] || { echo "memcheck FAILED (logs: $VG_LOG_DIR)" >&2; exit 1; }
        echo "memcheck passed"
        ;;
    coverage)
        result_begin coverage
        cmake_configure Debug "-DENABLE_COVERAGE=ON"
        cmake_build
        build_test_runner
        echo "Building PTY test binaries..."
        cmake --build "$BUILD_DIR" --target $PTY_TARGETS tests-functional -- -j"$JOBS"

        ABS_BUILD="$(realpath "$BUILD_DIR")"
        ROOT_ABS="$(pwd)"
        # A program that is killed while it writes its .gcda at exit leaves a
        # truncated file that fails lcov (EMAIL-119): the PTY harness lets a
        # child that is on its way out finish before it signals it.
        export PTYTEST_CLOSE_GRACE_MS="${PTYTEST_CLOSE_GRACE_MS:-3000}"
        # build/coverage/<scope>/ keeps the raw data of a scope for the
        # integrator (manage-sh-model.md): the .gcno and .gcda files of the
        # instrumented build tree, in the tree's own layout.  Counters are
        # zeroed before each scope; the snapshots themselves are left out.
        zero_counters() {
            find "$BUILD_DIR" -name '*.gcda' -not -path "$BUILD_DIR/coverage/*" -delete
        }
        snapshot_scope() {  # snapshot_scope <unit|external>
            local dest="$ABS_BUILD/coverage/$1"
            mkdir -p "$dest" || return 1
            find "$dest" -type f -delete || return 1
            (cd "$BUILD_DIR" && find . -path ./coverage -prune -o -type f \
                \( -name '*.gcno' -o -name '*.gcda' \) -exec cp --parents -t "$dest" {} +)
        }
        # One definition of "product code" for every report: the program's own
        # sources.  Test code, the PTY library and the system headers are not
        # measured (policies/testing-model.md, Coverage).
        capture_product() {  # capture_product <raw.info> <product.info>
            local dirs=() d
            # lcov recurses into a directory, so only the top-level ones are
            # named (a nested one as well gives "duplicate file").
            while IFS= read -r d; do dirs+=(--directory "$d"); done < <(cd "$BUILD_DIR" && \
                find . -name '*.gcno' -not -path './coverage/*' | cut -d/ -f2 | sort -u | \
                while IFS= read -r t; do [ -d "$t" ] && echo "$t"; done)
            (cd "$BUILD_DIR" && lcov --capture --ignore-errors empty "${dirs[@]}" --output-file "$1" && \
             lcov --extract "$1" --ignore-errors unused \
                  "$ROOT_ABS/src/*" "$ROOT_ABS/libemail/src/*" "$ROOT_ABS/libemail-client/src/*" \
                  --output-file "$2")
        }
        line_pct() {         # line_pct <product.info> -> "91.8"
            lcov --summary "$BUILD_DIR/$1" 2>&1 | grep 'lines' | grep -oP '[0-9]+\.[0-9]+(?=%)' | head -1
        }
        line_counts() {      # line_counts <product.info> -> "covered total"
            lcov --summary "$BUILD_DIR/$1" 2>&1 | grep 'lines' | grep -oP '\(\K[0-9]+ of [0-9]+' | head -1 | awk '{print $1, $3}'
        }

        # External scope: the functional, PTY and integration suites together
        # (black-box, instrumented binaries), from fresh counters.  The scripts
        # of the functional verb that rebuild the tree (the package tests) are
        # left to `functional`: they would replace the instrumented build.
        coverage_run_rc=0
        zero_counters
        R_PREFIX="external/"
        run_functional_suites "" 0 || coverage_run_rc=1
        run_pty_suites "" || coverage_run_rc=1
        run_integration_suites || coverage_run_rc=1
        gcda_rc=0
        tests/functional/gcda_check.sh "$ABS_BUILD" || gcda_rc=$?
        echo "Capturing external (functional + PTY + integration) coverage..."
        capture_product coverage-functional-raw.info coverage-functional.info
        snapshot_scope external || exit 1
        R_EXTRA+=("scope: external build/coverage/external $(line_counts coverage-functional.info)")

        # Unit tests on top of the same .gcda give the union of everything.
        # (LCOV 2.x --add-tracefile intersects lines instead of unioning; running
        # both kinds in sequence and capturing once gives the correct union.)
        (cd "$BUILD_DIR" && ./tests/unit/bin/test-runner) || coverage_run_rc=1
        echo "Capturing combined coverage..."
        capture_product coverage-raw.info coverage.info

        # Unit scope: from fresh counters, the unit tests have to stand on their own.
        zero_counters
        R_PREFIX="unit/"
        run_suite unit "$BUILD_DIR" ./tests/unit/bin/test-runner || coverage_run_rc=1
        echo "Capturing unit-only coverage..."
        capture_product coverage-unit-raw.info coverage-unit.info
        snapshot_scope unit || exit 1
        R_EXTRA+=("scope: unit build/coverage/unit $(line_counts coverage-unit.info)")

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
        echo "  unit tests alone                    ${UNIT_PCT}%   (minimum ${COVERAGE_MIN}%)"
        echo "  functional + PTY + integration      ${EXT_PCT}%   (minimum ${COVERAGE_MIN}%)"
        echo "  everything combined                 ${ALL_PCT}%   (target ${COVERAGE_TARGET}%)"
        # Kept for the per-release record (docs/dev/testing.md), so the figures
        # quoted anywhere are the ones of a run, with its date and commit.
        tests/functional/coverage_summary.sh "$(date +%F)" "$(git rev-parse --short=12 HEAD)" \
            "$BUILD_DIR/coverage-unit.info" "$BUILD_DIR/coverage-functional.info" \
            "$BUILD_DIR/coverage.info" | tee "$BUILD_DIR/coverage-summary.txt"
        echo "Combined report:  $BUILD_DIR/coverage_report/index.html"
        echo "External report:  $BUILD_DIR/coverage_functional_report/index.html"
        coverage_gate_rc=0
        for pair in "unit tests alone:$UNIT_PCT" "functional + PTY + integration:$EXT_PCT"; do
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
            echo "ERROR: coverage run failed: invalid .gcda files (named above, see build/result/log/)." >&2
            exit 1
        fi
        if [ "$coverage_run_rc" -ne 0 ]; then
            echo "ERROR: coverage run failed: a suite failed (named above; the reports were still produced)." >&2
            exit 1
        fi
        if [ "$coverage_gate_rc" -ne 0 ]; then
            exit 1
        fi
        ;;
    integration)
        result_begin integration
        build_release
        run_integration_suites || exit 1
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
        pkg_ver=$(sed -n 's/^set(CPACK_PACKAGE_VERSION "\(.*\)")$/\1/p' "$BUILD_DIR/CPackConfig.cmake")
        [ -n "$pkg_ver" ] || { echo "ERROR: no package version in $BUILD_DIR/CPackConfig.cmake." >&2; exit 1; }
        # package never tags and never pushes (EMAIL-241); tag-build VERSION tags.
        ;;
    uninstall)
        do_uninstall
        ;;
    tag-build)
        # EMAIL-229/241: local tag only, never pushed; the tag follows the package version, never decides it.
        tver="${2:-}"
        [ -n "$tver" ] || { echo "usage: manage.sh tag-build VERSION" >&2; exit 1; }
        tag_package_build "$tver" || exit 1
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
    help)
        show_help
        ;;
    *)
        show_help >&2
        exit 2
        ;;
esac
