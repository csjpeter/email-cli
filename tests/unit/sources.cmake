# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-12
# Source and include lists of the unit test runner (declared in tests/CMakeLists.txt).

set(UNIT_TEST_INCLUDES
    ${CMAKE_SOURCE_DIR}/libemail/src
    ${CMAKE_SOURCE_DIR}/libemail/src/core
    ${CMAKE_SOURCE_DIR}/libemail/src/infrastructure
    ${CMAKE_SOURCE_DIR}/libemail/src/domain
    ${CMAKE_SOURCE_DIR}/libemail/src/platform
    ${CMAKE_SOURCE_DIR}/libwrite/src
    ${CMAKE_SOURCE_DIR}/tests/common
)

# html_medium_stub.c replaces platform html_medium.c for deterministic tests.
set(UNIT_TEST_PLATFORM_SOURCES
    ${CMAKE_SOURCE_DIR}/libemail/src/platform/posix/terminal.c
    ${CMAKE_SOURCE_DIR}/libemail/src/platform/posix/path.c
    ${CMAKE_SOURCE_DIR}/libemail/src/platform/posix/process.c
    ${CMAKE_SOURCE_DIR}/libemail/src/platform/posix/credential_key.c
)

set(UNIT_TEST_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/unit/test_runner.c
    ${CMAKE_SOURCE_DIR}/tests/common/test_scratch.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_platform.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_fs_util.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_config.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_logger.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_wizard.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_imap_client.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_mime.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_local_store.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_imap_util.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_email_service.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_html_parser.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_html_render.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_input_line.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_tui_frame.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_compose_draft.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_path_complete.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_json_util.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_gmail_auth.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_gmail_client.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_label_idx.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_mail_client.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_gmail_sync.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_compose_service.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_smtp_adapter.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_when_expr.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_mail_rules.c
    ${CMAKE_SOURCE_DIR}/tests/unit/test_tuikit_link.c
    ${CMAKE_SOURCE_DIR}/libemail/src/infrastructure/when_expr.c
    ${CMAKE_SOURCE_DIR}/libwrite/src/compose_service.c
    ${CMAKE_SOURCE_DIR}/libwrite/src/smtp_adapter.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/fs_util.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/logger.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/config.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/mime_util.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/html_parser.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/html_render.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/imap_util.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/input_line.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/tui_frame.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/status_line.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/compose_draft.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/path_complete.c
    ${CMAKE_SOURCE_DIR}/libemail/src/core/json_util.c
    ${CMAKE_SOURCE_DIR}/libemail/src/infrastructure/gmail_auth.c
    ${CMAKE_SOURCE_DIR}/libemail/src/infrastructure/gmail_client.c
    ${CMAKE_SOURCE_DIR}/libemail/src/infrastructure/gmail_sync.c
    ${CMAKE_SOURCE_DIR}/libemail/src/infrastructure/mail_client.c
    ${CMAKE_SOURCE_DIR}/libemail/src/infrastructure/config_store.c
    ${CMAKE_SOURCE_DIR}/libemail/src/infrastructure/setup_wizard.c
    ${CMAKE_SOURCE_DIR}/libemail/src/infrastructure/imap_client.c
    ${CMAKE_SOURCE_DIR}/libemail/src/infrastructure/local_store.c
    ${CMAKE_SOURCE_DIR}/libemail/src/infrastructure/mail_rules.c
    ${CMAKE_SOURCE_DIR}/tests/unit/html_medium_stub.c
    ${UNIT_TEST_PLATFORM_SOURCES}
)
