# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Peter Csaszar (Császár Péter)
# Created: 2026-10-11
# CPack post-build script: stamps every .deb with XB-Git-Commit and
# XB-Build-Date control fields (packaging policy, "Identifying a build").
#
# CPack has no variable for extra control fields, so the package is unpacked,
# its control file extended and the package rebuilt.  Run by CPack through
# CPACK_POST_BUILD_SCRIPTS; CPACK_PACKAGE_FILES lists the packages and
# CPACK_EMAIL_SOURCE_DIR is the source tree the commit is read from.

find_program(_git git)
find_program(_dpkg_deb dpkg-deb)
if(NOT _git OR NOT _dpkg_deb)
    message(FATAL_ERROR "deb_build_info: git and dpkg-deb are required")
endif()

execute_process(
    COMMAND ${_git} rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CPACK_EMAIL_SOURCE_DIR}"
    OUTPUT_VARIABLE _commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0 OR _commit STREQUAL "")
    message(FATAL_ERROR "deb_build_info: cannot read the commit of ${CPACK_EMAIL_SOURCE_DIR}")
endif()
# A package built from uncommitted changes must not claim a clean commit.
execute_process(
    COMMAND ${_git} diff-index --quiet HEAD --
    WORKING_DIRECTORY "${CPACK_EMAIL_SOURCE_DIR}"
    RESULT_VARIABLE _dirty)
if(NOT _dirty EQUAL 0)
    set(_commit "${_commit}+dirty")
endif()
string(TIMESTAMP _date "%Y-%m-%dT%H:%M:%SZ" UTC)

# A package with a shared library gets an ldconfig trigger instead of the
# ldconfig calls CPack generates (Debian policy 8.1.1; lintian
# maintscript-calls-ldconfig).  The generated scripts are replaced only when
# they are exactly what CPack writes.
set(_ldconfig_postinst "#!/bin/sh\n\nset -e\n\nif [ \"$1\" = \"configure\" ]; then\n\tldconfig\nfi\n")
set(_ldconfig_postrm   "#!/bin/sh\n\nset -e\n\nif [ \"$1\" = \"remove\" ]; then\n\tldconfig\nfi\n")

foreach(_deb IN LISTS CPACK_PACKAGE_FILES)
    if(NOT _deb MATCHES "\\.deb$")
        continue()
    endif()
    set(_work "${CPACK_TOPLEVEL_DIRECTORY}/build-info-work")
    file(REMOVE_RECURSE "${_work}")
    execute_process(COMMAND ${_dpkg_deb} -R "${_deb}" "${_work}"
                    RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "deb_build_info: dpkg-deb -R ${_deb} failed (${_rc})")
    endif()
    file(READ "${_work}/DEBIAN/control" _control)
    string(REGEX REPLACE "\n+$" "\n" _control "${_control}")
    if(EXISTS "${_work}/DEBIAN/shlibs")
        foreach(_script postinst postrm)
            set(_path "${_work}/DEBIAN/${_script}")
            if(EXISTS "${_path}")
                file(READ "${_path}" _content)
                if(NOT _content STREQUAL "${_ldconfig_${_script}}")
                    message(FATAL_ERROR
                        "deb_build_info: ${_deb}: DEBIAN/${_script} is not the "
                        "ldconfig-only script CPack generates; refusing to replace it")
                endif()
                file(REMOVE "${_path}")
            endif()
        endforeach()
        file(WRITE "${_work}/DEBIAN/triggers" "activate-noawait ldconfig\n")
    endif()
    file(WRITE "${_work}/DEBIAN/control"
         "${_control}XB-Git-Commit: ${_commit}\nXB-Build-Date: ${_date}\n")
    execute_process(COMMAND ${_dpkg_deb} --root-owner-group -Zxz -b "${_work}" "${_deb}"
                    RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "deb_build_info: dpkg-deb -b ${_deb} failed (${_rc})")
    endif()
    file(REMOVE_RECURSE "${_work}")
endforeach()
