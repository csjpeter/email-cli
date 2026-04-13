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

# ── Custom target: run PTY view tests ─────────────────────────────────
add_custom_target(run-pty-views
    COMMAND test-pty-views
        $<TARGET_FILE:email-cli>
        $<TARGET_FILE:mock-imap-server>
        $<TARGET_FILE:email-cli-ro>
        $<TARGET_FILE:email-sync>
    DEPENDS test-pty-views email-cli email-cli-ro email-sync mock-imap-server
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
