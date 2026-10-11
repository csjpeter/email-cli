# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-03-30
# Build rules of the PTY terminal test library.

cmake_minimum_required(VERSION 3.10)
project(libptytest C)

set(CMAKE_C_STANDARD 11)

add_library(ptytest STATIC
    pty_session.c
    pty_screen.c
    pty_sync.c
)

target_include_directories(ptytest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Link against libutil (for forkpty/openpty)
target_link_libraries(ptytest PRIVATE util)

# Self-test binary
add_executable(test-ptytest test_ptytest.c)
target_link_libraries(test-ptytest ptytest)
target_link_libraries(test-ptytest util)
