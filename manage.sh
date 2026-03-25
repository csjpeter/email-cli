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
    echo "  deps           Install system dependencies (supports Ubuntu 24.04, Rocky 9)"
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

case "$1" in
    deps)
        install_deps
        ;;
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
