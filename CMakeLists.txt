cmake_minimum_required(VERSION 3.10)
project(email-cli VERSION 0.2.4 LANGUAGES C)

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

# ASAN Support in Debug builds (disabled when coverage is requested — ASAN+gcov slows PTY tests)
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ENABLE_COVERAGE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
endif()

# Coverage Support
if(ENABLE_COVERAGE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage -DENABLE_GCOV")
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
    COMMAND ${OPENSSL_EXECUTABLE} req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256
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

add_executable(email-import-rules src/main_import_rules.c)
target_link_libraries(email-import-rules libemail)

# Test Build
enable_testing()
add_subdirectory(tests/unit EXCLUDE_FROM_ALL)
add_subdirectory(tests/pty EXCLUDE_FROM_ALL)

# ── Install targets ────────────────────────────────────────────────────────────
install(TARGETS email-cli email-cli-ro email-sync email-tui email-import-rules
        RUNTIME DESTINATION bin)

# Man pages: section 1, one per binary.  Packagers compress them themselves.
install(FILES docs/man/email-cli.1
              docs/man/email-cli-ro.1
              docs/man/email-sync.1
              docs/man/email-tui.1
              docs/man/email-import-rules.1
        DESTINATION share/man/man1)

# ── CPack packaging (DEB + RPM) ────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME        "email-cli")
set(CPACK_PACKAGE_VERSION     "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT     "Peter Csaszar <peter@csaszar.email>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Terminal-based email client suite (IMAP + Gmail REST API)")
set(CPACK_PACKAGE_DESCRIPTION
"email-cli is a terminal-based email client suite written in C.
It supports standard IMAP servers and the native Gmail REST API (OAuth2).
Provides five binaries: email-cli (full read-write CLI),
email-cli-ro (read-only / scripting), email-tui (interactive TUI),
email-sync (background sync daemon), and email-import-rules.")
set(CPACK_RESOURCE_FILE_LICENSE    "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_STRIP_FILES              TRUE)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

# Debian-specific
set(CPACK_DEBIAN_PACKAGE_SECTION       "mail")
set(CPACK_DEBIAN_PACKAGE_PRIORITY      "optional")
set(CPACK_DEBIAN_PACKAGE_DEPENDS       "libssl3 (>= 3.0.0), libcurl4 (>= 7.68.0)")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER    "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_FILE_NAME             "DEB-DEFAULT")

# RPM-specific
set(CPACK_RPM_PACKAGE_LICENSE          "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP            "Applications/Internet")
set(CPACK_RPM_PACKAGE_REQUIRES         "openssl-libs >= 3.0.0, libcurl >= 7.68.0")
set(CPACK_RPM_FILE_NAME                "RPM-DEFAULT")

include(CPack)
