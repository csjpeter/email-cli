include_directories(
    ../../src/core
    ../../src/infrastructure
    ../../src/domain
    ../common
)

set(TEST_SOURCES
    test_runner.c
    test_fs_util.c
    test_config.c
    ../../src/core/fs_util.c
    ../../src/core/logger.c
    ../../src/infrastructure/config_store.c
)

add_executable(test-runner ${TEST_SOURCES})
target_link_libraries(test-runner) # Add libs if needed

add_test(NAME UnitTests COMMAND test-runner)
