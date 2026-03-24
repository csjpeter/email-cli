# email-cli Makefile
# Orchestrates build, diagnostics, and testing

BUILD_DIR = build
BIN_DIR = bin
PROJECT_NAME = email-cli

.PHONY: all build build-debug clean test-asan test-valgrind coverage

all: build

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
	@echo "Running tests with ASAN..."
	@# TODO: Add test execution command here when test-runner is ready
	@# $(BIN_DIR)/test-runner

test-valgrind: build
	@echo "Running tests with Valgrind..."
	@# TODO: Add valgrind command here
	@# valgrind --leak-check=full $(BIN_DIR)/test-runner

coverage: setup
	@cd $(BUILD_DIR) && cmake -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug ..
	@$(MAKE) -C $(BUILD_DIR)
	@echo "Running tests for coverage..."
	@# TODO: Run tests and generate lcov report
	@# lcov --capture --directory . --output-file coverage.info
	@# genhtml coverage.info --output-directory out

clean:
	@rm -rf $(BUILD_DIR)
	@rm -rf $(BIN_DIR)
	@echo "Cleaned."
