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

# The workspace's own libraries: a pinned submodule linked statically, or the
# installed -dev package (cmake/workspace_libs.cmake, WORKSPACE_LIBS).
include(cmake/workspace_libs.cmake)
# The programs are fully static (EMAIL-103), so the installed package is used
# through its static flavour too: no run-time dependency on libtuikit either way.
workspace_lib(libtuikit tuikit deps/libtuikit tuikit::tuikit_static tuikit::tuikit_static
              EMAIL_TUIKIT_LIB)
workspace_lib(libptytest ptytest deps/libptytest ptytest::ptytest ptytest::ptytest_static
              EMAIL_PTYTEST_LIB)
if(EMAIL_TUIKIT_LIB_WAY STREQUAL "bundled")
    add_subdirectory(deps/libtuikit EXCLUDE_FROM_ALL)
endif()
add_subdirectory(libemail-client)
add_subdirectory(libemail)

# The programs are fully static: one self-contained binary that runs on another
# Linux distribution with nothing installed beside it (EMAIL-103, owner decision
# 2026-10-16).  The libraries they use (libemail-client's static form, OpenSSL,
# libtuikit) are linked in, and so is the C library.
function(email_static_program name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} libemail)
    # AddressSanitizer cannot be linked static: the Debug build (the unit-test
    # run) keeps the C library dynamic; every other build is fully static.
    if(NOT (CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ENABLE_COVERAGE))
        target_link_options(${name} PRIVATE -static)
    endif()
endfunction()

email_static_program(email-cli src/main.c)
email_static_program(email-cli-ro src/main_ro.c)
email_static_program(email-sync src/main_sync.c)
email_static_program(email-tui src/main_tui.c)
email_static_program(email-import-rules src/main_import_rules.c)

# Test Build
enable_testing()
add_subdirectory(tests EXCLUDE_FROM_ALL)

# ── Install targets ────────────────────────────────────────────────────────────
install(TARGETS email-cli email-cli-ro email-sync email-tui email-import-rules
        RUNTIME DESTINATION bin COMPONENT cli)

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

install(FILES ${EMAIL_CLI_MAN_GZ} DESTINATION share/man/man1 COMPONENT cli)
install(FILES "${_changelog}.gz" DESTINATION share/doc/email-cli COMPONENT cli)
install(FILES packaging/copyright DESTINATION share/doc/email-cli COMPONENT cli)
# The XB-Git-Commit / XB-Build-Date fields are deliberate, not a defect; the
# override file states why, next to the package that carries them.
install(FILES packaging/lintian-overrides
        DESTINATION share/lintian/overrides RENAME email-cli COMPONENT cli)

# libemail-client: its own two packages (libemail-client0 and
# libemail-client-dev); each carries a changelog, the copyright file and the
# lintian override for the XB- build fields.  The man page is in -dev.
set(EMAIL_CLIENT_PKG_RUNTIME libemail-client0)
set(EMAIL_CLIENT_PKG_DEV     libemail-client-dev)
foreach(_pkg ${EMAIL_CLIENT_PKG_RUNTIME} ${EMAIL_CLIENT_PKG_DEV})
    set(_dir "${CMAKE_BINARY_DIR}/doc-${_pkg}")
    file(MAKE_DIRECTORY "${_dir}")
    file(WRITE "${_dir}/changelog"
"${_pkg} (${EMAIL_CLI_PACKAGE_VERSION}) unstable; urgency=medium

  * Build ${EMAIL_CLI_VERSION}.  The history is the git log of the
    repository; every change carries the key of its ticket.

 -- Péter Császár <csjpeter@gmail.com>  ${_changelog_date}
")
    add_custom_command(
        OUTPUT  "${_dir}/changelog.gz"
        COMMAND sh -c "\"$1\" -9n -c \"$2\" > \"$3\"" sh
                "${GZIP_EXECUTABLE}" "${_dir}/changelog" "${_dir}/changelog.gz"
        DEPENDS "${_dir}/changelog"
        VERBATIM)
    add_custom_target(changelog-${_pkg} ALL DEPENDS "${_dir}/changelog.gz")
    configure_file(packaging/lintian-overrides-lib.in
                   "${_dir}/lintian-overrides" @ONLY)
endforeach()
set(_emc_man "${CMAKE_BINARY_DIR}/man/libemail-client.3.gz")
add_custom_command(
    OUTPUT  "${_emc_man}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/man"
    COMMAND sh -c "\"$1\" -9n -c \"$2\" > \"$3\"" sh
            "${GZIP_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/docs/man/libemail-client.3" "${_emc_man}"
    DEPENDS "${CMAKE_SOURCE_DIR}/docs/man/libemail-client.3"
    VERBATIM)
add_custom_target(email-client-man ALL DEPENDS "${_emc_man}")
install(FILES "${_emc_man}" DESTINATION share/man/man3 COMPONENT dev)
foreach(_pair "runtime:${EMAIL_CLIENT_PKG_RUNTIME}" "dev:${EMAIL_CLIENT_PKG_DEV}")
    string(REPLACE ":" ";" _pair "${_pair}")
    list(GET _pair 0 _comp)
    list(GET _pair 1 _pkg)
    install(FILES "${CMAKE_BINARY_DIR}/doc-${_pkg}/changelog.gz" packaging/copyright
            DESTINATION share/doc/${_pkg} COMPONENT ${_comp})
    install(FILES "${CMAKE_BINARY_DIR}/doc-${_pkg}/lintian-overrides"
            DESTINATION share/lintian/overrides RENAME ${_pkg} COMPONENT ${_comp})
endforeach()

# Claude skill: tells an AI agent when and how to use the CLI
# (policies/cli-skill-model.md).  The package never writes into a home.
install(FILES claude/skills/email-cli/SKILL.md
        DESTINATION share/claude-skills/email-cli COMPONENT cli)

# ── CPack packaging (DEB + RPM) ────────────────────────────────────────────────
# Three packages from one build (policies/packaging-and-release-model.md,
# "Projects that ship more than one package"): the program suite, the shared
# library and its development files.
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_RPM_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL cli runtime dev)
set(CPACK_COMPONENTS_GROUPING IGNORE)
set(CPACK_PACKAGE_NAME        "email-cli")
set(CPACK_PACKAGE_VERSION     "${EMAIL_CLI_PACKAGE_VERSION}")
set(CPACK_PACKAGE_CONTACT     "Péter Császár <csjpeter@gmail.com>")
# No CPACK_PACKAGE_DESCRIPTION_SUMMARY: CPackDeb would prepend it to each
# component's description, and the first line of each is its own synopsis.
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

set(CPACK_DEBIAN_CLI_PACKAGE_NAME      "email-cli")
set(CPACK_DEBIAN_CLI_PACKAGE_SECTION   "mail")
set(CPACK_DEBIAN_CLI_FILE_NAME         "DEB-DEFAULT")
# The programs are fully static (EMAIL-103): they need no package at run time,
# so there is nothing to discover and nothing to name.  The library packages
# are for other programs.
set(CPACK_DEBIAN_CLI_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_CLI_PACKAGE_DEPENDS   "")
set(CPACK_COMPONENT_CLI_DESCRIPTION
"Terminal-based email client suite (IMAP + Gmail REST API)
email-cli is a terminal-based email client suite written in C.
It supports standard IMAP servers and the native Gmail REST API (OAuth2).
Provides five binaries: email-cli (full read-write CLI),
email-cli-ro (read-only / scripting), email-tui (interactive TUI),
email-sync (background sync daemon), and email-import-rules.")
# The shared library.  Its shlibs ask for at least the version, never exactly
# it (packaging policy, "Dependencies only ever look upward").
set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME    "${EMAIL_CLIENT_PKG_RUNTIME}")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "libs")
set(CPACK_DEBIAN_RUNTIME_FILE_NAME       "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS_POLICY ">=")
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION
"Mail client library of email-cli: IMAP, SMTP and the Gmail API
libemail-client holds the mail client logic of email-cli, so that other
programs send and read mail natively.  It audits nothing: it returns the
outcome and the message id to the caller.

This package contains the shared library.")
# The -dev package: from the same repository as the runtime package, so the
# exact dependency is the one the policy allows.
set(CPACK_DEBIAN_DEV_PACKAGE_NAME    "${EMAIL_CLIENT_PKG_DEV}")
set(CPACK_DEBIAN_DEV_PACKAGE_SECTION "libdevel")
set(CPACK_DEBIAN_DEV_PACKAGE_DEPENDS
    "${EMAIL_CLIENT_PKG_RUNTIME} (= ${EMAIL_CLI_PACKAGE_VERSION})")
set(CPACK_DEBIAN_DEV_FILE_NAME       "DEB-DEFAULT")
set(CPACK_COMPONENT_DEV_DESCRIPTION
"Mail client library of email-cli - development files
libemail-client holds the mail client logic of email-cli, so that other
programs send and read mail natively.

This package contains the header, the pkg-config file and the manual page.")

# RPM-specific
set(CPACK_RPM_PACKAGE_LICENSE          "GPL-3.0-only")
set(CPACK_RPM_PACKAGE_GROUP            "Applications/Internet")
set(CPACK_RPM_CLI_PACKAGE_REQUIRES "")
set(CPACK_RPM_RUNTIME_PACKAGE_REQUIRES "openssl-libs >= 3.0.0")
set(CPACK_RPM_DEV_PACKAGE_REQUIRES "libemail-client = ${EMAIL_CLI_PACKAGE_VERSION}")
set(CPACK_RPM_CLI_PACKAGE_SUMMARY "Terminal-based email client suite (IMAP + Gmail REST API)")
set(CPACK_RPM_RUNTIME_PACKAGE_SUMMARY "Mail client library of email-cli")
set(CPACK_RPM_DEV_PACKAGE_SUMMARY "Mail client library of email-cli - development files")
set(CPACK_RPM_CLI_PACKAGE_NAME "email-cli")
set(CPACK_RPM_RUNTIME_PACKAGE_NAME "libemail-client")
set(CPACK_RPM_DEV_PACKAGE_NAME "libemail-client-devel")
set(CPACK_RPM_FILE_NAME                "RPM-DEFAULT")

include(CPack)
