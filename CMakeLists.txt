# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-03-24
# Top-level CMake build: options, shared flags and the sub-projects of email-cli.

cmake_minimum_required(VERSION 3.20)
project(email-cli VERSION 0.3.0 LANGUAGES C)

# Version string: prefer "git describe", fall back to project version.
# Only release tags (v<digit>...) count: `--tags` alone accepts every tag, so a
# bookmark such as "before-some-work" would silently become the product version.
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --match "v[0-9]*" --always --dirty
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

# Package version, derived from the same describe.  Taking it from the
# hardcoded project() version instead lets packages go stale the moment a tag
# is added: the binary would report the tag while the .deb kept the old number.
#   v0.3.0               -> 0.3.0
#   v0.3.0-43-gabc1234   -> 0.3.0.43   (43 commits past the tag)
#   v0.1                 -> 0.1
# The "-dirty" marker stays in the binary's --version but is dropped here:
# dpkg and rpm both need a plain dotted number.
set(EMAIL_CLI_PACKAGE_VERSION "${PROJECT_VERSION}")
if(_GIT_DESCRIBE)
    set(_pkgver "${EMAIL_CLI_VERSION}")
    string(REGEX REPLACE "-dirty$" "" _pkgver "${_pkgver}")
    string(REGEX REPLACE "^([0-9]+(\\.[0-9]+)*)-([0-9]+)-g[0-9a-f]+$" "\\1.\\3"
           _pkgver "${_pkgver}")
    if(_pkgver MATCHES "^[0-9]+(\\.[0-9]+)*$")
        set(EMAIL_CLI_PACKAGE_VERSION "${_pkgver}")
    else()
        message(WARNING "Cannot derive a package version from '${_GIT_DESCRIBE}'; "
                        "falling back to ${PROJECT_VERSION}")
    endif()
endif()

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
add_subdirectory(tests EXCLUDE_FROM_ALL)

# ── Install targets ────────────────────────────────────────────────────────────
install(TARGETS email-cli email-cli-ro email-sync email-tui email-import-rules
        RUNTIME DESTINATION bin)

# Man pages: section 1, one per binary, installed gzipped (Debian policy 12.1;
# CPack does not compress them).  -n keeps the file name and time out of the
# archive so the package is reproducible.
find_program(GZIP_EXECUTABLE gzip REQUIRED)
set(EMAIL_CLI_MAN_PAGES email-cli email-cli-ro email-sync email-tui
                        email-import-rules)
set(EMAIL_CLI_MAN_GZ "")
foreach(_page IN LISTS EMAIL_CLI_MAN_PAGES)
    set(_src "${CMAKE_SOURCE_DIR}/docs/man/${_page}.1")
    set(_gz  "${CMAKE_BINARY_DIR}/man/${_page}.1.gz")
    add_custom_command(
        OUTPUT  "${_gz}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/man"
        COMMAND sh -c "\"$1\" -9n -c \"$2\" > \"$3\"" sh
                "${GZIP_EXECUTABLE}" "${_src}" "${_gz}"
        DEPENDS "${_src}"
        VERBATIM)
    list(APPEND EMAIL_CLI_MAN_GZ "${_gz}")
endforeach()

# Changelog: a native package's /usr/share/doc/<pkg>/changelog.gz, in Debian
# changelog format, generated for the version being built.
string(TIMESTAMP _changelog_date "%a, %d %b %Y %H:%M:%S +0000" UTC)
set(_changelog "${CMAKE_BINARY_DIR}/changelog")
file(WRITE "${_changelog}"
"email-cli (${EMAIL_CLI_PACKAGE_VERSION}) unstable; urgency=medium

  * Build ${EMAIL_CLI_VERSION}.  The history is the git log of the
    repository; every change carries the key of its ticket.

 -- Péter Császár <csjpeter@gmail.com>  ${_changelog_date}
")
add_custom_command(
    OUTPUT  "${_changelog}.gz"
    COMMAND sh -c "\"$1\" -9n -c \"$2\" > \"$3\"" sh
            "${GZIP_EXECUTABLE}" "${_changelog}" "${_changelog}.gz"
    DEPENDS "${_changelog}"
    VERBATIM)
add_custom_target(email-cli-docs ALL DEPENDS ${EMAIL_CLI_MAN_GZ} "${_changelog}.gz")

install(FILES ${EMAIL_CLI_MAN_GZ} DESTINATION share/man/man1)
install(FILES "${_changelog}.gz" DESTINATION share/doc/email-cli)
install(FILES packaging/copyright DESTINATION share/doc/email-cli)
# The XB-Git-Commit / XB-Build-Date fields are deliberate, not a defect; the
# override file states why, next to the package that carries them.
install(FILES packaging/lintian-overrides
        DESTINATION share/lintian/overrides RENAME email-cli)

# Claude skill: tells an AI agent when and how to use the CLI
# (policies/cli-skill-model.md).  The package never writes into a home.
install(FILES claude/skills/email-cli/SKILL.md
        DESTINATION share/claude-skills/email-cli)

# ── CPack packaging (DEB + RPM) ────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME        "email-cli")
set(CPACK_PACKAGE_VERSION     "${EMAIL_CLI_PACKAGE_VERSION}")
set(CPACK_PACKAGE_CONTACT     "Péter Császár <csjpeter@gmail.com>")
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
# Dependencies are discovered from the binaries, never written by hand.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS      ON)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER    "${CPACK_PACKAGE_CONTACT}")
set(CPACK_DEBIAN_FILE_NAME             "DEB-DEFAULT")
# XB-Git-Commit and XB-Build-Date: CPack has no control-field variable, so a
# post-build script adds them (packaging policy, "Identifying a build").
set(CPACK_EMAIL_SOURCE_DIR             "${CMAKE_SOURCE_DIR}")
set(CPACK_POST_BUILD_SCRIPTS           "${CMAKE_SOURCE_DIR}/cmake/deb_build_info.cmake")

# RPM-specific
set(CPACK_RPM_PACKAGE_LICENSE          "GPL-3.0-only")
set(CPACK_RPM_PACKAGE_GROUP            "Applications/Internet")
set(CPACK_RPM_PACKAGE_REQUIRES         "openssl-libs >= 3.0.0, libcurl >= 7.68.0")
set(CPACK_RPM_FILE_NAME                "RPM-DEFAULT")

include(CPack)
