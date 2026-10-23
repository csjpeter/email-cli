# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-04-13
# Build rules of libemail: core, platform, domain and infrastructure sources.

set(LIBEMAIL_PLATFORM_SOURCES
    src/platform/posix/terminal.c
    src/platform/posix/path.c
    src/platform/posix/html_medium.c
    src/platform/posix/process.c
    src/platform/posix/credential_key.c
)

add_library(libemail STATIC
    src/core/logger.c
    src/core/fs_util.c
    src/core/config.c
    src/core/charset.c
    src/core/mime_util.c
    src/core/html_parser.c
    src/core/html_render.c
    src/core/imap_util.c
    src/core/input_line.c
    src/core/key_events.c
    src/core/key_table.c
    src/core/status_line.c
    src/core/tui_frame.c
    src/core/body_editor.c
    src/core/compose_draft.c
    src/core/path_complete.c
    src/core/cli_table.c
    src/infrastructure/config_store.c
    src/infrastructure/gmail_login.c
    src/infrastructure/setup_wizard.c
    src/infrastructure/local_store.c
    src/infrastructure/when_expr.c
    src/infrastructure/mail_rules.c
    src/infrastructure/op_history.c
    src/domain/email_service.c
    src/domain/history_service.c
    src/domain/history_view.c
    ${LIBEMAIL_PLATFORM_SOURCES}
)

target_include_directories(libemail PUBLIC
    ${CMAKE_SOURCE_DIR}/libemail-client/include
    src
    src/core
    src/infrastructure
    src/domain
    src/platform
    ${OPENSSL_INCLUDE_DIR}
)

# The programs carry the mail client library inside (its static form) and need
# nothing at run time (EMAIL-103).
target_link_libraries(libemail PUBLIC email-client-static ${EMAIL_TUIKIT_LIB})
