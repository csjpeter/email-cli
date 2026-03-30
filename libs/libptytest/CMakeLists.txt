cmake_minimum_required(VERSION 3.10)
project(libptytest C)

set(CMAKE_C_STANDARD 11)

add_library(ptytest STATIC
    pty_session.c
    pty_screen.c
    pty_sync.c
)

target_include_directories(ptytest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Link against libutil on Linux (for forkpty/openpty)
if(NOT APPLE)
    target_link_libraries(ptytest PRIVATE util)
endif()

# Self-test binary
add_executable(test-ptytest test_ptytest.c)
target_link_libraries(test-ptytest ptytest)
if(NOT APPLE)
    target_link_libraries(test-ptytest util)
endif()
