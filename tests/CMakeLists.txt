# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-12
# Declaration of every test of the project (policies/testing-model.md, "How a
# test is declared, built and found"): one project_test() line per test.  The
# category and the name are the first two words; a test is built into
# build/tests/<category>/bin/<name>, binary or script.  ./manage.sh declared
# fails when a declared test has no executable there or an executable is not
# declared.

find_package(CURL REQUIRED)

# project_test(<category> <name> [SOURCES f...] [SCRIPT f] [INCLUDES d...]
#              [LINK l...] [NEEDS t...] [DEFINES d...] [TIMEOUT s])
#   SOURCES  compiled into build/tests/<category>/bin/<name>
#   SCRIPT   linked there as <name> (resolved against this directory)
#   NEEDS    targets built first (helpers, binaries the test drives)
#   DEFINES  extra compile definitions of this test
function(project_test category name)
    cmake_parse_arguments(PT "" "SCRIPT;TIMEOUT" "SOURCES;INCLUDES;LINK;NEEDS;DEFINES" ${ARGN})
    set(_bin "${CMAKE_BINARY_DIR}/tests/${category}/bin")
    if(PT_SCRIPT AND PT_SOURCES)
        message(FATAL_ERROR "project_test(${category} ${name}): SCRIPT or SOURCES, not both")
    endif()
    if(PT_SCRIPT)
        add_custom_command(OUTPUT "${_bin}/${name}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_bin}"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "${CMAKE_CURRENT_SOURCE_DIR}/${PT_SCRIPT}" "${_bin}/${name}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${PT_SCRIPT}"
            VERBATIM)
        add_custom_target(${name} DEPENDS "${_bin}/${name}")
    elseif(PT_SOURCES)
        add_executable(${name} EXCLUDE_FROM_ALL ${PT_SOURCES})
        set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${_bin}")
        target_include_directories(${name} PRIVATE ${PT_INCLUDES})
        target_compile_definitions(${name} PRIVATE
            TEST_BUILD_DIR="${CMAKE_BINARY_DIR}" TEST_CATEGORY="${category}")
        if(PT_DEFINES)
            target_compile_definitions(${name} PRIVATE ${PT_DEFINES})
        endif()
        if(PT_LINK)
            target_link_libraries(${name} ${PT_LINK})
        endif()
    else()
        message(FATAL_ERROR "project_test(${category} ${name}): SOURCES or SCRIPT needed")
    endif()
    if(PT_NEEDS)
        add_dependencies(${name} ${PT_NEEDS})
    endif()
    if(NOT TARGET tests-${category})
        add_custom_target(tests-${category})
    endif()
    add_dependencies(tests-${category} ${name})
    if(NOT TARGET tests)
        add_custom_target(tests)
    endif()
    add_dependencies(tests tests-${category})
endfunction()

include(unit/sources.cmake)
add_subdirectory(pty)   # helpers the PTY tests drive: mocks and the input-line harness

set(PTY_LIBS ptytest::ptytest_static util)
set(PTY_INCLUDES ${CMAKE_SOURCE_DIR}/tests/common)
set(SCRATCH ${CMAKE_SOURCE_DIR}/tests/common/test_scratch.c)

project_test(unit test-runner SOURCES ${UNIT_TEST_SOURCES} INCLUDES ${UNIT_TEST_INCLUDES} LINK OpenSSL::SSL OpenSSL::Crypto CURL::libcurl tuikit::tuikit_static NEEDS test-tls-certs
    DEFINES TEST_CERT_PATH="${CMAKE_BINARY_DIR}/tests/certs/test.crt" TEST_KEY_PATH="${CMAKE_BINARY_DIR}/tests/certs/test.key")

project_test(functional run_functional SCRIPT functional/run_functional.sh)
project_test(functional run_manage_sh SCRIPT functional/run_manage_sh.sh)
project_test(functional run_package_dir SCRIPT functional/run_package_dir.sh)
project_test(functional run_package_policy SCRIPT functional/run_package_policy.sh)
project_test(functional run_private_names SCRIPT functional/run_private_names.sh)
project_test(functional run_source_header SCRIPT functional/run_source_header.sh)
project_test(functional run_timing SCRIPT functional/run_timing.sh)
project_test(functional run_declared SCRIPT functional/run_declared.sh)

project_test(pty test-pty-views SOURCES pty/test_pty_views.c ${SCRATCH} INCLUDES ${PTY_INCLUDES} LINK ${PTY_LIBS} NEEDS email-cli email-cli-ro email-sync email-tui mock-imap-server)
project_test(pty test-pty-gmail-tui SOURCES pty/test_pty_gmail_tui.c ${SCRATCH} INCLUDES ${PTY_INCLUDES} LINK ${PTY_LIBS} NEEDS email-tui email-sync mock-gmail-server)
project_test(pty test-pty-mail-rules SOURCES pty/test_pty_mail_rules.c ${SCRATCH} INCLUDES ${PTY_INCLUDES} LINK ${PTY_LIBS} NEEDS email-sync email-tui mock-imap-server)
project_test(pty test-pty-compose-dialog SOURCES pty/test_pty_compose_dialog.c ${SCRATCH} INCLUDES ${PTY_INCLUDES} LINK ${PTY_LIBS} NEEDS email-tui mock-imap-server mock-smtp-server)
project_test(pty test-pty-attachment SOURCES pty/test_pty_attachment.c ${SCRATCH} INCLUDES ${PTY_INCLUDES} LINK ${PTY_LIBS} NEEDS email-tui mock-imap-server mock-smtp-server)
project_test(pty test-pty-send-local SOURCES pty/test_pty_send_local.c ${SCRATCH} INCLUDES ${PTY_INCLUDES} LINK ${PTY_LIBS} NEEDS email-tui email-sync mock-imap-server mock-smtp-server)
project_test(pty test-pty-input-line SOURCES pty/test_input_line_pty.c ${SCRATCH} INCLUDES ${PTY_INCLUDES} LINK ${PTY_LIBS} NEEDS input-line-harness)
project_test(pty test-pty-compose SOURCES pty/test_compose_pty.c ${SCRATCH} INCLUDES ${PTY_INCLUDES} LINK ${PTY_LIBS} NEEDS email-tui email-cli mock-smtp-server)

project_test(integration run_integration SCRIPT integration/run_integration.sh TIMEOUT 1800)
project_test(integration run_local_dovecot SCRIPT integration/run_local_dovecot.sh TIMEOUT 1800)
