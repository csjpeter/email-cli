if(WIN32)
    set(LIBEMAIL_PLATFORM_SOURCES
        src/platform/windows/terminal.c
        src/platform/windows/path.c
        src/platform/windows/html_medium.c
        src/platform/windows/process.c
    )
else()
    set(LIBEMAIL_PLATFORM_SOURCES
        src/platform/posix/terminal.c
        src/platform/posix/path.c
        src/platform/posix/html_medium.c
        src/platform/posix/process.c
    )
endif()

add_library(libemail STATIC
    src/core/logger.c
    src/core/fs_util.c
    src/core/config.c
    src/core/mime_util.c
    src/core/html_parser.c
    src/core/html_render.c
    src/core/imap_util.c
    src/core/input_line.c
    src/core/path_complete.c
    src/core/json_util.c
    src/infrastructure/config_store.c
    src/infrastructure/setup_wizard.c
    src/infrastructure/imap_client.c
    src/infrastructure/local_store.c
    src/infrastructure/gmail_auth.c
    src/infrastructure/gmail_client.c
    src/infrastructure/gmail_sync.c
    src/domain/email_service.c
    ${LIBEMAIL_PLATFORM_SOURCES}
)

target_include_directories(libemail PUBLIC
    src
    src/core
    src/infrastructure
    src/domain
    src/platform
    ${OPENSSL_INCLUDE_DIR}
)

find_package(CURL REQUIRED)
target_link_libraries(libemail PUBLIC OpenSSL::SSL OpenSSL::Crypto CURL::libcurl)
