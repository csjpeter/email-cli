# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-12
# Helpers the PTY tests drive: the mock servers and the input-line harness.
# The tests themselves are declared in tests/CMakeLists.txt.

# libptytest is a git submodule (libs/libptytest, pinned to a release tag).
# Used through add_subdirectory() it builds only the library; the static target
# is linked so the PTY suites carry no run-time dependency on it.
add_subdirectory(${CMAKE_SOURCE_DIR}/libs/libptytest libptytest)

add_executable(mock-imap-server EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/functional/mock_imap_server.c
)
add_dependencies(mock-imap-server test-tls-certs)
target_link_libraries(mock-imap-server OpenSSL::SSL OpenSSL::Crypto)

add_executable(mock-gmail-server EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/functional/mock_gmail_api_server.c
)

add_executable(mock-smtp-server EXCLUDE_FROM_ALL
    mock_smtp_server.c
)
add_dependencies(mock-smtp-server test-tls-certs)
target_link_libraries(mock-smtp-server OpenSSL::SSL OpenSSL::Crypto)

# Platform sources for the harness (same selection as the main build).
set(HARNESS_PLATFORM_SOURCES
    ${CMAKE_SOURCE_DIR}/libemail/src/platform/posix/terminal.c
    ${CMAKE_SOURCE_DIR}/libemail/src/platform/posix/path.c
)

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
