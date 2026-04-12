add_subdirectory(${CMAKE_SOURCE_DIR}/libs/libptytest libptytest)

# ── email-cli view tests (require mock IMAP server) ───────────────────
add_executable(test-pty-views test_pty_views.c)
target_link_libraries(test-pty-views ptytest)
if(NOT APPLE)
    target_link_libraries(test-pty-views util)
endif()

# ── Platform sources for harness (same selection as main build) ───────
if(WIN32)
    set(HARNESS_PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/src/platform/windows/terminal.c
        ${CMAKE_SOURCE_DIR}/src/platform/windows/path.c
    )
else()
    set(HARNESS_PLATFORM_SOURCES
        ${CMAKE_SOURCE_DIR}/src/platform/posix/terminal.c
        ${CMAKE_SOURCE_DIR}/src/platform/posix/path.c
    )
endif()

# ── InputLine / PathComplete harness ─────────────────────────────────
add_executable(input-line-harness EXCLUDE_FROM_ALL
    input_line_harness.c
    ${CMAKE_SOURCE_DIR}/src/core/input_line.c
    ${CMAKE_SOURCE_DIR}/src/core/path_complete.c
    ${CMAKE_SOURCE_DIR}/src/core/logger.c
    ${CMAKE_SOURCE_DIR}/src/core/fs_util.c
    ${HARNESS_PLATFORM_SOURCES}
)
target_include_directories(input-line-harness PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/platform
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
