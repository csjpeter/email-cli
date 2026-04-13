find_package(CURL REQUIRED)

add_library(libwrite STATIC
    src/smtp_adapter.c
    src/compose_service.c
)

target_include_directories(libwrite PUBLIC
    src
)

target_link_libraries(libwrite PUBLIC libemail CURL::libcurl)
