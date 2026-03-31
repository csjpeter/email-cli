add_subdirectory(${CMAKE_SOURCE_DIR}/libs/libptytest libptytest)

add_executable(test-pty-views test_pty_views.c)
target_link_libraries(test-pty-views ptytest)
if(NOT APPLE)
    target_link_libraries(test-pty-views util)
endif()
