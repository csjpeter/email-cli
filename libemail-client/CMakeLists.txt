# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-15
# Build rules of libemail-client: the shared library other programs link.

# The mail client logic of email-cli (IMAP, SMTP, Gmail REST API) compiled
# position independent into one shared library.  Only the emc_* functions of
# include/email-client.h are exported; everything else is hidden.  The
# library needs no terminal toolkit and reads no configuration file.
set(EMC_SRC ${CMAKE_SOURCE_DIR}/libemail/src)

# The ABI version, independent of the package version: it moves only when the
# interface breaks (packaging policy, "Which number moves").
set(EMAIL_CLIENT_SOVERSION 0)

add_library(email-client-objects OBJECT
    src/email_client.c
    src/mail_client.c
    src/imap_client.c
    src/gmail_client.c
    src/gmail_auth.c
    src/gmail_sync.c
    src/json_util.c
    src/smtp_adapter.c
    src/tls_revocation.c
    src/net_tls.c
    src/http_client.c
    src/compose_service.c
    ${EMC_SRC}/core/logger.c
    ${EMC_SRC}/core/fs_util.c
    ${EMC_SRC}/core/charset.c
    ${EMC_SRC}/core/mime_util.c
    ${EMC_SRC}/core/html_parser.c
    ${EMC_SRC}/core/html_render.c
    ${EMC_SRC}/core/imap_util.c
    ${EMC_SRC}/infrastructure/config_store.c
    ${EMC_SRC}/infrastructure/local_store.c
    ${EMC_SRC}/infrastructure/mail_rules.c
    ${EMC_SRC}/infrastructure/when_expr.c
    ${EMC_SRC}/core/config.c
    ${EMC_SRC}/platform/posix/path.c
    ${EMC_SRC}/platform/posix/process.c
    ${EMC_SRC}/platform/posix/credential_key.c
    ${EMC_SRC}/platform/posix/html_medium.c
)
target_include_directories(email-client-objects
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${EMC_SRC} ${EMC_SRC}/core ${EMC_SRC}/infrastructure
            ${EMC_SRC}/domain ${EMC_SRC}/platform)
target_compile_definitions(email-client-objects PRIVATE
    EMC_VERSION_STRING="${EMAIL_CLI_PACKAGE_VERSION}")
set_target_properties(email-client-objects PROPERTIES
    C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON)
target_link_libraries(email-client-objects PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# The library in both forms, from the same objects (local-service-access-model:
# "How a client library is built and linked").  The shared one is what other
# programs link; the static one is what the email programs link, so that the
# programs are one self-contained binary (EMAIL-103).  The library needs only
# OpenSSL: there is no libcurl.
add_library(email-client SHARED $<TARGET_OBJECTS:email-client-objects>)
set_target_properties(email-client PROPERTIES
    OUTPUT_NAME email-client
    VERSION     ${EMAIL_CLIENT_SOVERSION}.0.0
    SOVERSION   ${EMAIL_CLIENT_SOVERSION})
target_include_directories(email-client PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(email-client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_link_options(email-client PRIVATE "LINKER:--as-needed" "LINKER:-z,defs")

# libssl.a and libcrypto.a of the system: the static form of the library, and
# every program that links it, carry OpenSSL inside.
find_library(EMAIL_SSL_STATIC    NAMES libssl.a    REQUIRED)
find_library(EMAIL_CRYPTO_STATIC NAMES libcrypto.a REQUIRED)
find_library(EMAIL_ZLIB_STATIC   NAMES libz.a      REQUIRED)   # libcrypto.a uses it
find_library(EMAIL_ZSTD_STATIC   NAMES libzstd.a   REQUIRED)   # and this
set(EMAIL_STATIC_TLS_LIBS ${EMAIL_SSL_STATIC} ${EMAIL_CRYPTO_STATIC} ${EMAIL_ZLIB_STATIC}
    ${EMAIL_ZSTD_STATIC}
    ${CMAKE_DL_LIBS} pthread CACHE INTERNAL "static OpenSSL and what it needs")
add_library(email-client-static STATIC $<TARGET_OBJECTS:email-client-objects>)
set_target_properties(email-client-static PROPERTIES OUTPUT_NAME email-client)
target_include_directories(email-client-static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(email-client-static PUBLIC ${EMAIL_STATIC_TLS_LIBS})

# Packages are laid out for /usr (CPACK_PACKAGING_INSTALL_PREFIX), and Debian
# keeps libraries in a multiarch directory; lintian wants the pkg-config file
# beside them.  Taken from dpkg, "lib" where there is none.
set(EMC_PREFIX /usr)
execute_process(COMMAND dpkg-architecture -qDEB_HOST_MULTIARCH
                OUTPUT_VARIABLE EMC_MULTIARCH OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE _emc_rc)
if(_emc_rc EQUAL 0 AND EMC_MULTIARCH)
    set(EMC_LIBDIR lib/${EMC_MULTIARCH})
else()
    set(EMC_LIBDIR lib)
endif()
# pkg-config file of the -dev package.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/libemail-client.pc.in
               ${CMAKE_BINARY_DIR}/libemail-client.pc @ONLY)

install(TARGETS email-client
        LIBRARY DESTINATION ${EMC_LIBDIR}
                COMPONENT runtime
                NAMELINK_COMPONENT dev)
install(TARGETS email-client-static
        ARCHIVE DESTINATION ${EMC_LIBDIR}
                COMPONENT dev)
install(FILES include/email-client.h
        DESTINATION include COMPONENT dev)
install(FILES ${CMAKE_BINARY_DIR}/libemail-client.pc
        DESTINATION ${EMC_LIBDIR}/pkgconfig COMPONENT dev)
