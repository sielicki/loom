# SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

# Recreate the same dependency target for source and installed-package builds.
function(loom_find_libfabric minimum_version static_link)
    set(LOOM_LIBFABRIC_FOUND OFF PARENT_SCOPE)
    set(LOOM_LIBFABRIC_ERROR "libfabric ${minimum_version} or newer is required" PARENT_SCOPE)

    if(static_link)
        set(prefix LIBFABRIC_STATIC)
        # Function scope keeps --static out of other dependency lookups.
        list(APPEND PKG_CONFIG_ARGN --static)
    else()
        set(prefix LIBFABRIC)
    endif()
    pkg_check_modules(${prefix} QUIET IMPORTED_TARGET "libfabric>=${minimum_version}")
    if(NOT ${prefix}_FOUND)
        return()
    endif()

    if(static_link)
        # pkg-config --static supplies private dependencies; it does not select
        # an archive when both shared and static libfabric are installed.
        set(CMAKE_FIND_LIBRARY_SUFFIXES "${CMAKE_STATIC_LIBRARY_SUFFIX}")
        set(archive archive-NOTFOUND)
        find_library(archive NAMES fabric
            HINTS ${${prefix}_LIBRARY_DIRS} "${${prefix}_LIBDIR}"
            NO_DEFAULT_PATH NO_CACHE)
        if(NOT archive)
            set(LOOM_LIBFABRIC_ERROR
                "Static libfabric ${minimum_version} or newer was requested, but its archive was not found"
                PARENT_SCOPE)
            return()
        endif()
        set(libraries ${${prefix}_LIBRARIES})
        if(NOT "fabric" IN_LIST libraries)
            set(LOOM_LIBFABRIC_ERROR "libfabric pkg-config metadata does not identify the fabric library" PARENT_SCOPE)
            return()
        endif()
        list(TRANSFORM libraries REPLACE "^fabric$" "${archive}")
        set_target_properties(PkgConfig::${prefix} PROPERTIES
            INTERFACE_LINK_LIBRARIES "${libraries}"
            INTERFACE_LINK_DIRECTORIES "${${prefix}_LIBRARY_DIRS}")
    endif()

    set(LOOM_LIBFABRIC_TARGET PkgConfig::${prefix} PARENT_SCOPE)
    set(LOOM_LIBFABRIC_VERSION "${${prefix}_VERSION}" PARENT_SCOPE)
    set(LOOM_LIBFABRIC_FOUND ON PARENT_SCOPE)
endfunction()
