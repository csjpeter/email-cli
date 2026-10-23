# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-23
# Derives the package version: project() version plus the total commit count.

# EMAIL-229 (packaging-and-release-model.md, "Deriving the version"): the
# version is <project version>.<git rev-list --count HEAD>.  A tag takes no
# part in it and restarts nothing; the tag vX.Y.Z.N is made after the package
# build, following the version.  With no git (or no history) the version is the
# bare project version.  The build needs a full clone, not a shallow one.
#
#   email_cli_derive_version(<project version> <source dir> <out variable>)
#
# Run as a script (cmake -DPROJECT_VERSION=V -DSRC=DIR -P derive_version.cmake)
# it prints the version, which is how the test checks it.
function(email_cli_derive_version project_version src_dir out_var)
    set(_ver "${project_version}")
    find_program(_git git)
    if(_git)
        execute_process(
            COMMAND ${_git} rev-list --count HEAD
            WORKING_DIRECTORY "${src_dir}"
            OUTPUT_VARIABLE _count
            RESULT_VARIABLE _rc
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(_rc EQUAL 0 AND _count MATCHES "^[0-9]+$")
            set(_ver "${project_version}.${_count}")
        endif()
    endif()
    set(${out_var} "${_ver}" PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    email_cli_derive_version("${PROJECT_VERSION}" "${SRC}" _out)
    execute_process(COMMAND ${CMAKE_COMMAND} -E echo "${_out}")
endif()
