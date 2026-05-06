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
    echo "  valgrind       Build and run unit tests with Valgrind"
    echo "  coverage       Run tests and generate coverage report"
    echo "  integration    Run integration test against Dovecot IMAP container"
    echo "  integration-local  Run APPEND integration test with local Dovecot (no Docker)"
    echo "  imap-down      Stop integration test container (preserves emails volume)"
    echo "  imap-clean     Remove integration test container and volume"
    echo "  install        Build (release) and install binaries to ~/.local/bin"
    echo "  uninstall      Remove installed binaries from ~/.local/bin"
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
                    sudo apt-get install -y build-essential cmake libcurl4-openssl-dev libssl-dev lcov valgrind
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
                    sudo dnf install -y cmake libcurl-devel openssl-devel lcov valgrind
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
    cmake -DCMAKE_BUILD_TYPE="$build_type" \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
          $extra_flags ..
    cd ..
    # Symlink compile_commands.json to project root for clangd/LSP
    ln -sf build/compile_commands.json compile_commands.json
}

JOBS=$(nproc)

cmake_build() {
    cmake --build "$BUILD_DIR" -- -j"$JOBS"
    cp "$BUILD_DIR/$PROJECT_NAME" "$BIN_DIR/"
    cp "$BUILD_DIR/${PROJECT_NAME}-ro" "$BIN_DIR/" 2>/dev/null || true
    cp "$BUILD_DIR/email-sync"         "$BIN_DIR/" 2>/dev/null || true
    cp "$BUILD_DIR/email-tui"          "$BIN_DIR/" 2>/dev/null || true
    cp "$BUILD_DIR/email-import-rules" "$BIN_DIR/" 2>/dev/null || true
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
        cmake --build "$BUILD_DIR" \
            --target test-pty-views --target mock-imap-server \
            --target mock-smtp-server --target test-pty-compose \
            --target test-pty-send-local -- -j"$JOBS"

        # Pass 1 — functional suite + PTY tests (fresh .gcda) → functional badge
        find "$BUILD_DIR" -name "*.gcda" -delete
        # Kill any lingering mock server processes by process name (not by -f to avoid self-kill)
        pkill "mock_imap_server" 2>/dev/null || true
        pkill "mock-imap-server" 2>/dev/null || true
        sleep 0.3
        ./tests/functional/run_functional.sh || true
        echo "Running PTY tests for coverage..."
        ABS_BUILD="$(realpath "$BUILD_DIR")"
        ABS_BIN="$(realpath "$BIN_DIR")"
        # PTY tests must run from the build directory so mock-imap-server finds
        # tests/certs/test.crt relative to cwd.
        (cd "$ABS_BUILD" && ./tests/pty/test-pty-views \
            "$ABS_BIN/email-cli" \
            ./tests/pty/mock-imap-server \
            "$ABS_BIN/email-cli-ro" \
            "$ABS_BIN/email-sync" \
            "$ABS_BIN/email-tui" \
            2>/dev/null) || true
        (cd "$ABS_BUILD" && ./tests/pty/test-pty-compose \
            "$ABS_BIN/email-tui" \
            ./tests/pty/mock-smtp-server \
            "$ABS_BIN/email-cli" \
            2>/dev/null) || true
        (cd "$ABS_BUILD" && ./tests/pty/test-pty-send-local \
            "$ABS_BIN/email-tui" \
            "$ABS_BIN/email-sync" \
            ./tests/pty/mock-imap-server \
            ./tests/pty/mock-smtp-server \
            2>/dev/null) || true
        echo "Capturing functional coverage..."
        (cd "$BUILD_DIR" && lcov --capture --directory . \
             --output-file coverage-functional-raw.info && \
         lcov --remove coverage-functional-raw.info \
              --ignore-errors unused \
              '*/src/main_tui.c' \
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
              '*/src/main_tui.c' \
              --output-file coverage.info)

        echo "Generating coverage reports..."
        (cd "$BUILD_DIR" && \
         genhtml coverage.info --output-directory coverage_report && \
         genhtml coverage-functional.info --output-directory coverage_functional_report)
        echo "Combined coverage:    $BUILD_DIR/coverage_report/index.html"
        echo "Functional coverage:  $BUILD_DIR/coverage_functional_report/index.html"
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
    install)
        do_install
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
