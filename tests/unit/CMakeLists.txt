include_directories(
    ../../libemail/src
    ../../libemail/src/core
    ../../libemail/src/infrastructure
    ../../libemail/src/domain
    ../common
)

# Platform-specific sources for tests
# html_medium_stub.c replaces platform html_medium.c for deterministic tests
if(WIN32)
    set(TEST_PLATFORM_SOURCES
        ../../libemail/src/platform/windows/terminal.c
        ../../libemail/src/platform/windows/path.c
        ../../libemail/src/platform/windows/process.c
    )
else()
    set(TEST_PLATFORM_SOURCES
        ../../libemail/src/platform/posix/terminal.c
        ../../libemail/src/platform/posix/path.c
        ../../libemail/src/platform/posix/process.c
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
    test_input_line.c
    test_path_complete.c
    test_json_util.c
    test_gmail_auth.c
    test_gmail_client.c
    test_label_idx.c
    ../../libemail/src/core/fs_util.c
    ../../libemail/src/core/logger.c
    ../../libemail/src/core/config.c
    ../../libemail/src/core/mime_util.c
    ../../libemail/src/core/html_parser.c
    ../../libemail/src/core/html_render.c
    ../../libemail/src/core/imap_util.c
    ../../libemail/src/core/input_line.c
    ../../libemail/src/core/path_complete.c
    ../../libemail/src/core/json_util.c
    ../../libemail/src/infrastructure/gmail_auth.c
    ../../libemail/src/infrastructure/gmail_client.c
    ../../libemail/src/infrastructure/config_store.c
    ../../libemail/src/infrastructure/setup_wizard.c
    ../../libemail/src/infrastructure/imap_client.c
    ../../libemail/src/infrastructure/local_store.c
    html_medium_stub.c
    ${TEST_PLATFORM_SOURCES}
)

add_executable(test-runner ${TEST_SOURCES})
find_package(CURL REQUIRED)
target_link_libraries(test-runner OpenSSL::SSL OpenSSL::Crypto CURL::libcurl)
add_dependencies(test-runner test-tls-certs)

add_test(NAME UnitTests COMMAND test-runner)
