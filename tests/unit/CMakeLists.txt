include_directories(
    ../../src
    ../../src/core
    ../../src/infrastructure
    ../../src/domain
    ../common
)

# Platform-specific sources for tests
# html_medium_stub.c replaces platform html_medium.c for deterministic tests
if(WIN32)
    set(TEST_PLATFORM_SOURCES
        ../../src/platform/windows/terminal.c
        ../../src/platform/windows/path.c
        ../../src/platform/windows/process.c
    )
else()
    set(TEST_PLATFORM_SOURCES
        ../../src/platform/posix/terminal.c
        ../../src/platform/posix/path.c
        ../../src/platform/posix/process.c
    )
endif()

set(TEST_SOURCES
    test_runner.c
    test_platform.c
    test_fs_util.c
    test_config.c
    test_logger.c
    test_wizard.c
    test_imap_client.c
    test_mime.c
    test_local_store.c
    test_imap_util.c
    test_email_service.c
    test_html_parser.c
    test_html_render.c
    ../../src/core/fs_util.c
    ../../src/core/logger.c
    ../../src/core/config.c
    ../../src/core/mime_util.c
    ../../src/core/html_parser.c
    ../../src/core/html_render.c
    ../../src/core/imap_util.c
    ../../src/core/input_line.c
    ../../src/core/path_complete.c
    ../../src/infrastructure/config_store.c
    ../../src/infrastructure/setup_wizard.c
    ../../src/infrastructure/imap_client.c
    ../../src/infrastructure/local_store.c
    html_medium_stub.c
    ${TEST_PLATFORM_SOURCES}
)

add_executable(test-runner ${TEST_SOURCES})
target_link_libraries(test-runner OpenSSL::SSL OpenSSL::Crypto)

add_test(NAME UnitTests COMMAND test-runner)
