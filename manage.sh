#!/bin/bash

# email-cli management script
# A user-friendly interface for building, testing, and running the project

# Exit on error
set -e

PROJECT_NAME="email-cli"
BIN_PATH="./bin/$PROJECT_NAME"

show_help() {
    echo "Usage: ./manage.sh [command]"
    echo ""
    echo "Commands:"
    echo "  build          Build the project in Release mode"
    echo "  debug          Build the project in Debug mode (with ASAN)"
    echo "  run            Build and run the application"
    echo "  test           Build and run unit tests (with ASAN)"
    echo "  valgrind       Build and run unit tests with Valgrind"
    echo "  coverage       Run tests and generate coverage report"
    echo "  clean-logs     Purge all application log files"
    echo "  clean          Remove all build artifacts"
    echo "  help           Show this help message"
}

case "$1" in
    build)
        make build
        ;;
    debug)
        make build-debug
        ;;
    run)
        make build
        echo "Launching $PROJECT_NAME..."
        $BIN_PATH
        ;;
    test)
        make test-asan
        ;;
    valgrind)
        make test-valgrind
        ;;
    coverage)
        make coverage
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
        make clean
        ;;
    help|*)
        show_help
        ;;
esac
