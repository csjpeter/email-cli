#!/bin/bash

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
    echo "  test           Build and run unit tests (with ASAN)"
    echo "  functional     Build and run the functional test suite"
    echo "  pty            Build and run all PTY (terminal) test suites"
    echo "  valgrind       Build and run unit tests with Valgrind"
    echo "  check          Run every gate (test, functional, pty, valgrind) and summarise"
    echo "  coverage       Run tests and generate coverage report"
    echo "  integration    Run integration test against Dovecot IMAP container"
    echo "  integration-local  Run APPEND integration test with local Dovecot (no Docker)"
    echo "  imap-down      Stop integration test container (preserves emails volume)"
    echo "  imap-clean     Remove integration test container and volume"
    echo "  install        Build (release) and install binaries to ~/.local/bin"
    echo "  uninstall      Remove installed binaries from ~/.local/bin"
    echo "  package [deb|rpm|all]  Build release and create package(s) in build/packages/"
    echo "  clean-logs     Purge all application log files"
    echo "  clean          Remove all build artifacts"
    echo "  help           Show this help message"
}

install_deps() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        case "$ID" in
            ubuntu)
                if [[ "$VERSION_ID" == "24.04" ]]; then
                    echo "Detected Ubuntu 24.04. Installing dependencies..."
                    sudo apt-get update
                    sudo apt-get install -y build-essential cmake libcurl4-openssl-dev libssl-dev lcov valgrind dpkg-dev
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
                    sudo dnf install -y cmake libcurl-devel openssl-devel lcov valgrind rpm-build
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
    mkdir -p "$BUILD_DIR" "$BIN_DIR"
    cd "$BUILD_DIR"
    # Coverage is a per-invocation choice, not a sticky cache entry.  Without an
    # explicit OFF here a previous `coverage` run leaves instrumentation enabled
    # in CMakeCache.txt, and every later build — release binaries and packages
    # included — silently carries gcov instrumentation.  $extra_flags comes
    # last so the coverage target can still turn it back on.
    cmake -DCMAKE_BUILD_TYPE="$build_type" \
          -DENABLE_COVERAGE=OFF \
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
    "$run" views          ./tests/pty/test-pty-views "$B/email-cli" \
                          ./tests/pty/mock-imap-server "$B/email-cli-ro" \
                          "$B/email-sync" "$B/email-tui"
    "$run" gmail-tui      ./tests/pty/test-pty-gmail-tui "$B/email-tui" \
                          "$B/email-sync" ./tests/pty/mock-gmail-server
    "$run" mail-rules     ./tests/pty/test-pty-mail-rules "$B/email-sync" \
                          "$B/email-tui" ./tests/pty/mock-imap-server
    "$run" compose-dialog ./tests/pty/test-pty-compose-dialog "$B/email-tui" \
                          ./tests/pty/mock-imap-server ./tests/pty/mock-smtp-server
    "$run" attachment     ./tests/pty/test-pty-attachment "$B/email-tui" \
                          ./tests/pty/mock-imap-server ./tests/pty/mock-smtp-server
    "$run" send-local     ./tests/pty/test-pty-send-local "$B/email-tui" \
                          "$B/email-sync" ./tests/pty/mock-imap-server \
                          ./tests/pty/mock-smtp-server
    "$run" input-line     ./tests/pty/test-pty-input-line \
                          ./tests/pty/input-line-harness
    "$run" compose        ./tests/pty/test-pty-compose "$B/email-tui" \
                          ./tests/pty/mock-smtp-server "$B/email-cli"
    # The PTY harness itself.  Its self-test existed but was never wired into
    # any target, so a break in the library the other eight suites depend on
    # would have surfaced as confusing failures in those suites instead.
    "$run" ptytest        ./tests/pty/libptytest/test-ptytest
}

PTY_TARGETS="test-pty-views test-pty-gmail-tui test-pty-mail-rules \
             test-pty-compose-dialog test-pty-attachment test-pty-send-local \
             test-pty-compose test-pty-input-line test-ptytest \
             mock-imap-server mock-gmail-server mock-smtp-server \
             input-line-harness"

JOBS=$(nproc)

# Stops mock servers an interrupted run left behind.  pkill exits 1 when
# nothing matched, which is the normal case and is handled explicitly; any
# other status is a real failure and is shown.  The fixed mock ports that make
# this necessary are EMAIL-33's to remove.
stop_stale_mocks() {
    local pat rc
    for pat in mock_imap_server mock-imap-server mock_gmail_api_server \
               mock-gmail-server mock_smtp_server mock-smtp-server; do
        pkill -f "$pat" && rc=0 || rc=$?
        if [ "$rc" -gt 1 ]; then
            echo "ERROR: pkill -f $pat failed with status $rc" >&2
            exit 1
        fi
    done
}


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
    cmake --build "$BUILD_DIR" --target test-runner -- -j"$JOBS"
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
    echo "Uninstall complete."
}

case "$1" in
    deps)
        install_deps
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
        (cd "$BUILD_DIR" && ./tests/unit/test-runner)
        ;;
    functional)
        echo "Running functional tests..."
        build_release
        ./tests/functional/run_functional.sh
        ;;
    pty)
        echo "Running PTY tests..."
        build_release
        cmake --build "$BUILD_DIR" --target $PTY_TARGETS -- -j"$JOBS"
        ABS_BUILD="$(realpath "$BUILD_DIR")"
        ABS_BIN="$(realpath "$BIN_DIR")"
        # Mock servers bind fixed ports.  A previous interrupted run can leave
        # one behind, and the next suite then talks to a stale server with
        # different contents — which looks exactly like a product regression.
        stop_stale_mocks
        sleep 1
        pty_rc=0
        run_pty_strict() {  # run_pty_strict <label> <binary> [args…]
            local label="$1"; shift
            echo "--- PTY: $label ---"
            # Each suite binds fixed mock-server ports; run them sequentially
            # and let the ports settle in between.
            if ! (cd "$ABS_BUILD" && "$@"); then
                echo "  [FAILED] $label"
                pty_rc=1
            fi
            sleep 2
        }
        for_each_pty_suite run_pty_strict "$ABS_BIN"
        exit $pty_rc
        ;;
    valgrind)
        echo "Running unit tests with Valgrind..."
        build_release
        build_test_runner
        (cd "$BUILD_DIR" && valgrind --leak-check=full --error-exitcode=1 \
            --child-silent-after-fork=yes ./tests/unit/test-runner)
        ;;
    coverage)
        cmake_configure Debug "-DENABLE_COVERAGE=ON"
        cmake_build
        build_test_runner
        echo "Building PTY test binaries..."
        cmake --build "$BUILD_DIR" --target $PTY_TARGETS -- -j"$JOBS"

        # Pass 1 — functional suite + PTY tests (fresh .gcda) → functional badge
        find "$BUILD_DIR" -name "*.gcda" -delete
        # Kill any lingering mock server processes by process name (not by -f to avoid self-kill)
        stop_stale_mocks
        sleep 0.3
        # The report is still produced when the suite fails, but the failure is
        # the verdict of this run: it is kept and returned at the end.
        functional_rc=0
        ./tests/functional/run_functional.sh || functional_rc=$?
        pty_cov_rc=0
        echo "Running PTY tests for coverage..."
        ABS_BUILD="$(realpath "$BUILD_DIR")"
        ABS_BIN="$(realpath "$BIN_DIR")"
        # The functional suite above runs a mock on port 9993 — the same fixed
        # port three PTY suites use, and the mock's own default.  Any server it
        # leaves behind answers the next suite's connect probe, which then
        # tests against a mailbox belonging to someone else: that is how
        # mail-rules came to assert over messages titled "AlphaAccountMsg".
        # `./manage.sh pty` has cleared these since the same bug bit it; the
        # coverage path needs it just as much, because here the functional run
        # immediately precedes the PTY run.
        stop_stale_mocks
        sleep 1
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
            sleep 2
        }
        for_each_pty_suite run_pty_lenient "$ABS_BIN"
        echo "Capturing functional coverage..."
        (cd "$BUILD_DIR" && lcov --capture --directory . \
             --output-file coverage-functional-raw.info && \
         lcov --remove coverage-functional-raw.info \
              --ignore-errors unused \
              '/usr/include/*' \
              '*/src/main_tui.c' \
              '*/tests/unit/*' \
              --output-file coverage-functional.info)

        # Pass 2 — run unit suite ON TOP of existing functional .gcda → combined badge
        # (LCOV 2.x --add-tracefile intersects lines instead of unioning; running both
        # test suites in sequence and capturing once gives the correct union.)
        (cd "$BUILD_DIR" && ./tests/unit/test-runner)
        echo "Capturing combined (unit + functional) coverage..."
        (cd "$BUILD_DIR" && lcov --capture --directory . \
             --output-file coverage-raw.info && \
         lcov --remove coverage-raw.info \
              --ignore-errors unused \
              '/usr/include/*' \
              '*/src/main_tui.c' \
              --output-file coverage.info)

        echo "Generating coverage reports..."
        (cd "$BUILD_DIR" && \
         genhtml coverage.info --output-directory coverage_report && \
         genhtml coverage-functional.info --output-directory coverage_functional_report)
        FUNC_SUMMARY=$(lcov --summary "$BUILD_DIR/coverage-functional.info" 2>&1 | grep 'functions')
        FUNC_PCT=$(echo "$FUNC_SUMMARY" | grep -oP '[0-9]+\.[0-9]+(?=%)')
        LINE_SUMMARY=$(lcov --summary "$BUILD_DIR/coverage.info" 2>&1 | grep 'functions')
        LINE_PCT=$(echo "$LINE_SUMMARY" | grep -oP '[0-9]+\.[0-9]+(?=%)')
        echo "Functional badge: ${FUNC_PCT}%  (function coverage)"
        echo "Combined badge:   ${LINE_PCT}%  (function coverage, unit+functional)"
        echo "Combined coverage:    $BUILD_DIR/coverage_report/index.html"
        echo "Functional coverage:  $BUILD_DIR/coverage_functional_report/index.html"
        if [ "$functional_rc" -ne 0 ] || [ "$pty_cov_rc" -ne 0 ]; then
            echo "ERROR: coverage run failed: functional suite status $functional_rc," \
                 "PTY suites status $pty_cov_rc (reports above were still produced)." >&2
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
    imap-down)
        ./tests/integration/run_integration.sh --down
        ;;
    imap-clean)
        ./tests/integration/run_integration.sh --clean
        ;;
    check)
        # Every gate, each in its own run, one summary to compare with CI.
        # Coverage joins when it has a threshold that fails (EMAIL-15).
        check_rc=0
        check_summary=""
        for gate in test functional pty valgrind; do
            echo "=== check: $gate ==="
            if "$0" "$gate"; then
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
        if ! ( cd "$BUILD_DIR" && cpack -G "$generators" --config CPackConfig.cmake -B "$PKG_DIR" ); then
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
        echo "Package(s) written to $PKG_DIR/"
        ls -lh "$PKG_DIR"
        ;;
    uninstall)
        do_uninstall
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
