# email-cli Makefile
# Orchestrates build, diagnostics, and testing

BUILD_DIR = build
BIN_DIR = bin
PROJECT_NAME = email-cli

.PHONY: all build build-debug clean test-asan test-valgrind test-functional coverage help

# Default target: show help
all: help

help:
	@echo "email-cli build system"
	@echo ""
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  build           Build production binary (Release)"
	@echo "  build-debug     Build diagnostic binary (Debug + ASAN)"
	@echo "  test-asan       Run unit tests with Address Sanitizer"
	@echo "  test-valgrind   Run unit tests with Valgrind leak detection"
	@echo "  test-functional Run end-to-end tests against mock server"
	@echo "  coverage        Run tests and generate GCOV coverage report"
	@echo "  clean           Remove build and bin directories"
	@echo "  help            Show this help message"

setup:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BIN_DIR)

build: setup
	@cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=Release ..
	@$(MAKE) -C $(BUILD_DIR)
	@cp $(BUILD_DIR)/$(PROJECT_NAME) $(BIN_DIR)/
	@echo "Build complete: $(BIN_DIR)/$(PROJECT_NAME)"

build-debug: setup
	@cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=Debug ..
	@$(MAKE) -C $(BUILD_DIR)
	@cp $(BUILD_DIR)/$(PROJECT_NAME) $(BIN_DIR)/
	@echo "Debug build (with ASAN) complete: $(BIN_DIR)/$(PROJECT_NAME)"

test-asan: build-debug
	@echo "Running unit tests with ASAN..."
	@$(MAKE) -C $(BUILD_DIR) test-runner
	@$(BUILD_DIR)/tests/unit/test-runner

test-valgrind: build
	@echo "Running unit tests with Valgrind..."
	@$(MAKE) -C $(BUILD_DIR) test-runner
	@valgrind --leak-check=full --error-exitcode=1 $(BUILD_DIR)/tests/unit/test-runner

test-functional: build
	@echo "Running functional tests..."
	@./tests/functional/run_functional.sh

coverage: setup
	@cd $(BUILD_DIR) && cmake -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug ..
	@$(MAKE) -C $(BUILD_DIR)
	@$(MAKE) -C $(BUILD_DIR) test-runner
	@$(BUILD_DIR)/tests/unit/test-runner
	@./tests/functional/run_functional.sh
	@echo "Generating coverage report..."
	@# Note: requires lcov installed
	@lcov --capture --directory . --output-file $(BUILD_DIR)/coverage.info
	@genhtml $(BUILD_DIR)/coverage.info --output-directory $(BUILD_DIR)/coverage_report
	@echo "Coverage report available at $(BUILD_DIR)/coverage_report/index.html"

clean:
	@rm -rf $(BUILD_DIR)
	@rm -rf $(BIN_DIR)
	@echo "Cleaned."
