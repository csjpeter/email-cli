# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-04-13
# Build rules of libwrite: SMTP adapter and compose service.

find_package(CURL REQUIRED)

add_library(libwrite STATIC
    src/smtp_adapter.c
    src/compose_service.c
)

target_include_directories(libwrite PUBLIC
    src
)

target_link_libraries(libwrite PUBLIC libemail CURL::libcurl)
