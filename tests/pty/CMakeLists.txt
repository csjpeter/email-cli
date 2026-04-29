add_subdirectory(${CMAKE_SOURCE_DIR}/libs/libptytest libptytest)

# ── email-cli view tests (require mock IMAP server) ───────────────────
add_executable(test-pty-views test_pty_views.c)
target_link_libraries(test-pty-views ptytest)
if(NOT APPLE)
    target_link_libraries(test-pty-views util)
endif()

# ── Mock IMAP server (built inline for PTY tests) ─────────────────────
add_executable(mock-imap-server EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/functional/mock_imap_server.c
)
add_dependencies(mock-imap-server test-tls-certs)
target_link_libraries(mock-imap-server OpenSSL::SSL OpenSSL::Crypto)

# ── Custom target: run PTY view tests ─────────────────────────────────
add_custom_target(run-pty-views
    COMMAND test-pty-views
        $<TARGET_FILE:email-cli>
        $<TARGET_FILE:mock-imap-server>
        $<TARGET_FILE:email-cli-ro>
        $<TARGET_FILE:email-sync>
        $<TARGET_FILE:email-tui>
    DEPENDS test-pty-views email-cli email-cli-ro email-sync mock-imap-server email-tui
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running PTY view tests"
)

# ── Platform sources for harness (same selection as main build) ───────
if(WIN32)
    set(HARNESS_PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/libemail/src/platform/windows/terminal.c
        ${CMAKE_SOURCE_DIR}/libemail/src/platform/windows/path.c
    )
else()
    set(HARNESS_PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/libemail/src/platform/posix/terminal.c
        ${CMAKE_SOURCE_DIR}/libemail/src/platform/posix/path.c
    )
endif()

# ── InputLine / PathComplete harness ─────────────────────────────────
add_executable(input-line-harness EXCLUDE_FROM_ALL
    input_line_harness.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/input_line.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/path_complete.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/logger.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/fs_util.c
    ${HARNESS_PLATFORM_SOURCES}
)
target_include_directories(input-line-harness PRIVATE
    ${CMAKE_SOURCE_DIR}/libemail/src
    ${CMAKE_SOURCE_DIR}/libemail/src/core
    ${CMAKE_SOURCE_DIR}/libemail/src/platform
)
target_link_libraries(input-line-harness OpenSSL::SSL OpenSSL::Crypto)

# ── InputLine / PathComplete PTY tests ───────────────────────────────
add_executable(test-pty-input-line EXCLUDE_FROM_ALL
    test_input_line_pty.c
)
target_include_directories(test-pty-input-line PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/libptytest
)
target_link_libraries(test-pty-input-line ptytest)
if(NOT APPLE)
    target_link_libraries(test-pty-input-line util)
endif()
add_dependencies(test-pty-input-line input-line-harness)

# ── Mock Gmail API server ─────────────────────────────────────────────
add_executable(mock-gmail-server EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/functional/mock_gmail_api_server.c
)

# ── Gmail TUI PTY tests ───────────────────────────────────────────────
add_executable(test-pty-gmail-tui EXCLUDE_FROM_ALL
    test_pty_gmail_tui.c
)
target_include_directories(test-pty-gmail-tui PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/libptytest
)
target_link_libraries(test-pty-gmail-tui ptytest)
if(NOT APPLE)
    target_link_libraries(test-pty-gmail-tui util)
endif()
add_dependencies(test-pty-gmail-tui email-tui email-sync mock-gmail-server)

add_custom_target(run-pty-gmail-tui
    COMMAND test-pty-gmail-tui
        $<TARGET_FILE:email-tui>
        $<TARGET_FILE:email-sync>
        $<TARGET_FILE:mock-gmail-server>
    DEPENDS test-pty-gmail-tui email-tui email-sync mock-gmail-server
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running Gmail TUI PTY tests"
)

# ── Mock SMTP server ──────────────────────────────────────────────────
add_executable(mock-smtp-server EXCLUDE_FROM_ALL
    mock_smtp_server.c
)
add_dependencies(mock-smtp-server test-tls-certs)
target_link_libraries(mock-smtp-server OpenSSL::SSL OpenSSL::Crypto)

# ── Compose / send PTY tests ──────────────────────────────────────────
add_executable(test-pty-compose EXCLUDE_FROM_ALL
    test_compose_pty.c
)
target_include_directories(test-pty-compose PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/libptytest
)
target_link_libraries(test-pty-compose ptytest)
if(NOT APPLE)
    target_link_libraries(test-pty-compose util)
endif()
add_dependencies(test-pty-compose email-tui mock-smtp-server email-cli)

add_custom_target(run-pty-compose
    COMMAND test-pty-compose
        $<TARGET_FILE:email-tui>
        $<TARGET_FILE:mock-smtp-server>
        $<TARGET_FILE:email-cli>
    DEPENDS test-pty-compose email-tui mock-smtp-server email-cli
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running PTY compose/send tests"
)

# ── Mail rules PTY tests ──────────────────────────────────────────────
add_executable(test-pty-mail-rules EXCLUDE_FROM_ALL
    test_pty_mail_rules.c
)
target_include_directories(test-pty-mail-rules PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/libptytest
)
target_link_libraries(test-pty-mail-rules ptytest)
if(NOT APPLE)
    target_link_libraries(test-pty-mail-rules util)
endif()
add_dependencies(test-pty-mail-rules email-sync email-tui mock-imap-server)

add_custom_target(run-pty-mail-rules
    COMMAND test-pty-mail-rules
        $<TARGET_FILE:email-sync>
        $<TARGET_FILE:email-tui>
        $<TARGET_FILE:mock-imap-server>
    DEPENDS test-pty-mail-rules email-sync email-tui mock-imap-server
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running PTY mail rules tests"
)

# ── Pre-compose dialog PTY tests ──────────────────────────────────────
add_executable(test-pty-compose-dialog EXCLUDE_FROM_ALL
    test_pty_compose_dialog.c
)
target_include_directories(test-pty-compose-dialog PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/libptytest
)
target_link_libraries(test-pty-compose-dialog ptytest)
if(NOT APPLE)
    target_link_libraries(test-pty-compose-dialog util)
endif()
add_dependencies(test-pty-compose-dialog email-tui mock-imap-server mock-smtp-server)

add_custom_target(run-pty-compose-dialog
    COMMAND test-pty-compose-dialog
        $<TARGET_FILE:email-tui>
        $<TARGET_FILE:mock-imap-server>
        $<TARGET_FILE:mock-smtp-server>
    DEPENDS test-pty-compose-dialog email-tui mock-imap-server mock-smtp-server
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running PTY pre-compose dialog tests"
)

# ── Local-first send PTY tests (US-SL-01 … US-SL-04) ─────────────────
add_executable(test-pty-send-local EXCLUDE_FROM_ALL
    test_pty_send_local.c
)
target_include_directories(test-pty-send-local PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/libptytest
)
target_link_libraries(test-pty-send-local ptytest)
if(NOT APPLE)
    target_link_libraries(test-pty-send-local util)
endif()
add_dependencies(test-pty-send-local email-tui email-sync mock-imap-server mock-smtp-server)

add_custom_target(run-pty-send-local
    COMMAND test-pty-send-local
        $<TARGET_FILE:email-tui>
        $<TARGET_FILE:email-sync>
        $<TARGET_FILE:mock-imap-server>
        $<TARGET_FILE:mock-smtp-server>
    DEPENDS test-pty-send-local email-tui email-sync mock-imap-server mock-smtp-server
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running PTY local-first send tests"
)
