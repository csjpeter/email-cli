cmake_minimum_required(VERSION 3.10)
project(email-cli C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -pedantic -D_GNU_SOURCE")

# ASAN Support in Debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
endif()

# Coverage Support
if(ENABLE_COVERAGE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
endif()

find_package(CURL REQUIRED)

include_directories(
    src
    src/core
    src/infrastructure
    src/domain
    ${CURL_INCLUDE_DIRS}
)

# Platform-specific sources (CMake selects, no #ifdef in shared code)
if(WIN32)
    set(PLATFORM_SOURCES
        src/platform/windows/terminal.c
        src/platform/windows/path.c
        src/platform/windows/html_medium.c
    )
else()
    set(PLATFORM_SOURCES
        src/platform/posix/terminal.c
        src/platform/posix/path.c
        src/platform/posix/html_medium.c
    )
endif()

# Application Source Files
set(SOURCES
    src/main.c
    src/core/logger.c
    src/core/fs_util.c
    src/core/config.c
    src/core/mime_util.c
    src/core/html_parser.c
    src/core/html_render.c
    src/core/imap_util.c
    src/infrastructure/config_store.c
    src/infrastructure/setup_wizard.c
    src/infrastructure/curl_adapter.c
    src/infrastructure/local_store.c
    src/domain/email_service.c
    ${PLATFORM_SOURCES}
)

add_executable(email-cli ${SOURCES})
target_link_libraries(email-cli ${CURL_LIBRARIES})

# Test Build
enable_testing()
add_subdirectory(tests/unit EXCLUDE_FROM_ALL)
add_subdirectory(tests/pty EXCLUDE_FROM_ALL)
