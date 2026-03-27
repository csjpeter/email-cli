include_directories(
    ../../src
    ../../src/core
    ../../src/infrastructure
    ../../src/domain
    ../common
)

# Platform-specific sources for tests
if(WIN32)
    set(TEST_PLATFORM_SOURCES
        ../../src/platform/windows/terminal.c
        ../../src/platform/windows/path.c
    )
else()
    set(TEST_PLATFORM_SOURCES
        ../../src/platform/posix/terminal.c
        ../../src/platform/posix/path.c
    )
endif()

set(TEST_SOURCES
    test_runner.c
    test_platform.c
    test_fs_util.c
    test_config.c
    test_logger.c
    test_wizard.c
    test_curl.c
    test_mime.c
    test_cache.c
    test_imap_util.c
    ../../src/core/fs_util.c
    ../../src/core/logger.c
    ../../src/core/config.c
    ../../src/core/mime_util.c
    ../../src/core/imap_util.c
    ../../src/infrastructure/config_store.c
    ../../src/infrastructure/setup_wizard.c
    ../../src/infrastructure/curl_adapter.c
    ../../src/infrastructure/cache_store.c
    ${TEST_PLATFORM_SOURCES}
)

add_executable(test-runner ${TEST_SOURCES})
target_link_libraries(test-runner ${CURL_LIBRARIES})

add_test(NAME UnitTests COMMAND test-runner)
