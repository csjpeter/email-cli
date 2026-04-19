#!/bin/bash

# email-cli management script
# A user-friendly interface for building, testing, and running the project

set -e

PROJECT_NAME="email-cli"
BUILD_DIR="./build"
BIN_DIR="./bin"
BIN_PATH="$BIN_DIR/$PROJECT_NAME"

INSTALL_DIR="${HOME}/.local/bin"
INSTALL_BINS="email-cli email-cli-ro email-sync email-tui"

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
    cmake -DCMAKE_BUILD_TYPE="$build_type" $extra_flags ..
    cd ..
}

JOBS=$(nproc)

cmake_build() {
    cmake --build "$BUILD_DIR" -- -j"$JOBS"
    cp "$BUILD_DIR/$PROJECT_NAME" "$BIN_DIR/"
    cp "$BUILD_DIR/${PROJECT_NAME}-ro" "$BIN_DIR/" 2>/dev/null || true
    cp "$BUILD_DIR/email-sync"         "$BIN_DIR/" 2>/dev/null || true
    cp "$BUILD_DIR/email-tui"          "$BIN_DIR/" 2>/dev/null || true
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
        (cd "$BUILD_DIR" && valgrind --leak-check=full --error-exitcode=1 ./tests/unit/test-runner)
        ;;
    coverage)
        cmake_configure Debug "-DENABLE_COVERAGE=ON"
        cmake_build
        build_test_runner
        # Remove stale .gcda files to avoid checksum mismatch errors
        find "$BUILD_DIR" -name "*.gcda" -delete
        (cd "$BUILD_DIR" && ./tests/unit/test-runner)
        ./tests/functional/run_functional.sh
        echo "Generating coverage report..."
        (cd "$BUILD_DIR" && \
         lcov --capture --directory . --output-file coverage.info && \
         genhtml coverage.info --output-directory coverage_report)
        echo "Coverage report available at $BUILD_DIR/coverage_report/index.html"
        ;;
    integration)
        build_release
        ./tests/integration/run_integration.sh
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
        rm -rf "$BUILD_DIR" "$BIN_DIR"
        echo "Cleaned."
        ;;
    help|*)
        show_help
        ;;
esac
