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

find_package(OpenSSL REQUIRED)

add_subdirectory(libemail)
add_subdirectory(libwrite)

add_executable(email-cli src/main.c)
target_link_libraries(email-cli libemail)

add_executable(email-cli-ro src/main_ro.c)
target_link_libraries(email-cli-ro libemail)

add_executable(email-sync src/main_sync.c)
target_link_libraries(email-sync libemail)

add_executable(email-tui src/main_tui.c)
target_link_libraries(email-tui libemail libwrite)

# Test Build
enable_testing()
add_subdirectory(tests/unit EXCLUDE_FROM_ALL)
add_subdirectory(tests/pty EXCLUDE_FROM_ALL)
