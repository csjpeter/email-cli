# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-12
# Selects how the workspace's own libraries come in: bundled or system.

# The workspace's own libraries (libtuikit, libptytest) come in one of two ways
# (policies/dependencies-model.md, "two ways, both supported"), chosen by
# WORKSPACE_LIBS=bundled|system|auto:
#
#   bundled  the git submodule under deps/, built with the program and linked
#            statically; the binaries need nothing of ours at run time.
#   system   the installed -dev package, found with find_package() and linked
#            dynamically; the package then depends on the runtime package.
#   auto     the installed -dev package when it is there, else the submodule;
#            neither: a fatal error that names both ways.
#
# Sets EMAIL_TUIKIT_LIB and EMAIL_PTYTEST_LIB to the target to link, and
# EMAIL_TUIKIT_WAY / EMAIL_PTYTEST_WAY to the way each one resolved to.
set(WORKSPACE_LIBS "bundled" CACHE STRING
    "How the workspace libraries come in: bundled (submodule, static), system (-dev package) or auto")
set_property(CACHE WORKSPACE_LIBS PROPERTY STRINGS bundled system auto)
if(NOT WORKSPACE_LIBS MATCHES "^(bundled|system|auto)$")
    message(FATAL_ERROR
        "WORKSPACE_LIBS='${WORKSPACE_LIBS}': use bundled (submodule under deps/), "
        "system (installed -dev package) or auto")
endif()

# workspace_lib(<name> <package> <submodule dir> <system target> <bundled target>
#               <result var>)
function(workspace_lib name package subdir system_target bundled_target result)
    set(_way "${WORKSPACE_LIBS}")
    set(_have_sub FALSE)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${subdir}/CMakeLists.txt")
        set(_have_sub TRUE)
    endif()
    if(_way STREQUAL "auto")
        find_package(${package} CONFIG QUIET)
        if(${package}_FOUND)
            set(_way system)
        elseif(_have_sub)
            set(_way bundled)
        else()
            message(FATAL_ERROR
                "${name} not found. Either install its -dev package (WORKSPACE_LIBS=system) "
                "or fetch the submodule: git submodule update --init --recursive "
                "(WORKSPACE_LIBS=bundled; ./manage.sh build does it for you).")
        endif()
    endif()
    if(_way STREQUAL "system")
        find_package(${package} CONFIG REQUIRED)
        set(_target "${system_target}")
    else()
        if(NOT _have_sub)
            message(FATAL_ERROR
                "${name}: the submodule ${subdir} is missing. Run ./manage.sh build "
                "(it fetches submodules), or git submodule update --init --recursive, "
                "or build against the installed package with -DWORKSPACE_LIBS=system.")
        endif()
        # Used through add_subdirectory() a library builds only itself;
        # EXCLUDE_FROM_ALL keeps its unused flavour out of the default build.
        # The subdirectory is added once, by the first caller (tests/pty adds
        # libptytest itself, in its own binary directory).
        set(_target "${bundled_target}")
    endif()
    set(${result} "${_target}" PARENT_SCOPE)
    set(${result}_WAY "${_way}" PARENT_SCOPE)
    message(STATUS "${name}: ${_way} (${_target})")
endfunction()
