cmake_minimum_required(VERSION 3.10)
project(email-cli VERSION 0.1.5 LANGUAGES C)

# Version string: prefer "git describe", fall back to project version
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE _GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(_GIT_DESCRIBE)
    string(REGEX REPLACE "^v" "" EMAIL_CLI_VERSION "${_GIT_DESCRIBE}")
else()
    set(EMAIL_CLI_VERSION "${PROJECT_VERSION}")
endif()
add_compile_definitions(EMAIL_CLI_VERSION="${EMAIL_CLI_VERSION}")

# Gmail OAuth2 credentials (optional — loaded from gmail_credentials.cmake)
if(EXISTS "${CMAKE_SOURCE_DIR}/gmail_credentials.cmake")
    include("${CMAKE_SOURCE_DIR}/gmail_credentials.cmake")
endif()
if(GMAIL_DEFAULT_CLIENT_ID)
    add_compile_definitions(GMAIL_DEFAULT_CLIENT_ID="${GMAIL_DEFAULT_CLIENT_ID}")
endif()
if(GMAIL_DEFAULT_CLIENT_SECRET)
    add_compile_definitions(GMAIL_DEFAULT_CLIENT_SECRET="${GMAIL_DEFAULT_CLIENT_SECRET}")
endif()

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

# ── Test TLS certificates (self-signed, localhost only) ───────────────
find_program(OPENSSL_EXECUTABLE openssl REQUIRED)
set(TEST_CERT_DIR ${CMAKE_BINARY_DIR}/tests/certs)
set(TEST_CERT ${TEST_CERT_DIR}/test.crt)
set(TEST_KEY  ${TEST_CERT_DIR}/test.key)
add_custom_command(
    OUTPUT ${TEST_CERT} ${TEST_KEY}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${TEST_CERT_DIR}
    COMMAND ${OPENSSL_EXECUTABLE} req -x509 -newkey rsa:2048
            -keyout ${TEST_KEY}
            -out ${TEST_CERT}
            -days 3650 -nodes
            -subj "/CN=localhost"
    COMMENT "Generating test TLS certificates (self-signed, localhost)"
    VERBATIM
)
add_custom_target(test-tls-certs DEPENDS ${TEST_CERT} ${TEST_KEY})

add_subdirectory(libemail)
add_subdirectory(libwrite)

add_executable(email-cli src/main.c)
target_link_libraries(email-cli libemail libwrite)

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
