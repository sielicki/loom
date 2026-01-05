# SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
# Modern CMake configuration for loom - C++23 libfabric bindings

cmake_minimum_required(VERSION 3.28)

project(loom
    VERSION 0.1.0
    LANGUAGES CXX C
    DESCRIPTION "Modern C++23 bindings for libfabric with stdexec/asio integration"
    HOMEPAGE_URL "https://github.com/sielicki/loom"
)

# =============================================================================
# Project-wide settings
# =============================================================================

# Require C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Disable C++20 modules scanning (we don't use modules yet)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Generate compile_commands.json for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release build if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Position independent code (required for shared libraries)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# =============================================================================
# Options
# =============================================================================

# Build options
option(BUILD_SHARED_LIBS "Build shared libraries instead of static" OFF)
option(LOOM_BUILD_TESTS "Build tests" ON)
option(LOOM_BUILD_EXAMPLES "Build examples" OFF)
option(LOOM_BUILD_RUST_BINDINGS "Build Rust bindings" OFF)

# Linking options
option(LOOM_LIBFABRIC_STATIC "Link libfabric statically" OFF)

# Optional integrations
option(LOOM_USE_STDEXEC "Enable stdexec integration" OFF)
option(LOOM_USE_ASIO "Enable Asio integration" OFF)
option(LOOM_USE_BEMAN_EXECUTION "Enable beman::execution integration" OFF)

# FetchContent options (for when dependencies aren't found)
option(LOOM_FETCHCONTENT_STDEXEC "Allow FetchContent for stdexec if not found" OFF)
option(LOOM_FETCHCONTENT_ASIO "Allow FetchContent for Asio if not found" OFF)
option(LOOM_FETCHCONTENT_BEMAN_EXECUTION "Allow FetchContent for beman::execution if not found" OFF)

# Developer options
option(LOOM_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(LOOM_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(LOOM_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(LOOM_ENABLE_MSAN "Enable MemorySanitizer" OFF)
option(LOOM_ENABLE_IWYU "Enable include-what-you-use" OFF)
option(LOOM_ENABLE_COVERAGE "Enable code coverage" OFF)
option(LOOM_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

# =============================================================================
# CMake module path
# =============================================================================

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# =============================================================================
# Compiler configuration
# =============================================================================

# Create an interface library for common compile options
add_library(loom_compile_options INTERFACE)
add_library(loom::compile_options ALIAS loom_compile_options)

# Compiler warnings
if(MSVC)
    target_compile_options(loom_compile_options INTERFACE
        /W4
        $<$<BOOL:${LOOM_WARNINGS_AS_ERRORS}>:/WX>
    )
else()
    target_compile_options(loom_compile_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wcast-align
        -Wunused
        $<$<BOOL:${LOOM_WARNINGS_AS_ERRORS}>:-Werror>
    )
endif()

# Sanitizers (mutually exclusive in most cases)
if(LOOM_ENABLE_ASAN)
    target_compile_options(loom_compile_options INTERFACE
        -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(loom_compile_options INTERFACE -fsanitize=address)
    message(STATUS "AddressSanitizer enabled")
endif()

if(LOOM_ENABLE_UBSAN)
    target_compile_options(loom_compile_options INTERFACE
        -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(loom_compile_options INTERFACE -fsanitize=undefined)
    message(STATUS "UndefinedBehaviorSanitizer enabled")
endif()

if(LOOM_ENABLE_TSAN)
    target_compile_options(loom_compile_options INTERFACE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(loom_compile_options INTERFACE -fsanitize=thread)
    message(STATUS "ThreadSanitizer enabled")
endif()

if(LOOM_ENABLE_MSAN)
    target_compile_options(loom_compile_options INTERFACE
        -fsanitize=memory -fno-omit-frame-pointer)
    target_link_options(loom_compile_options INTERFACE -fsanitize=memory)
    message(STATUS "MemorySanitizer enabled")
endif()

# Include-what-you-use
if(LOOM_ENABLE_IWYU)
    find_program(IWYU_PROGRAM NAMES include-what-you-use iwyu)
    if(IWYU_PROGRAM)
        message(STATUS "Found include-what-you-use: ${IWYU_PROGRAM}")
        set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE
            "${IWYU_PROGRAM}"
            "-Xiwyu" "--no_fwd_decls"
            "-Xiwyu" "--cxx17ns"
            "-Xiwyu" "--max_line_length=100"
        )
    else()
        message(WARNING "LOOM_ENABLE_IWYU is ON but include-what-you-use was not found")
    endif()
endif()

# Code coverage
if(LOOM_ENABLE_COVERAGE)
    include(CodeCoverage)
endif()

# =============================================================================
# Dependencies
# =============================================================================

find_package(Threads REQUIRED)
find_package(PkgConfig)

# -----------------------------------------------------------------------------
# libfabric
# -----------------------------------------------------------------------------

# Try to find libfabric
set(LOOM_LIBFABRIC_FOUND OFF)

if(PKG_CONFIG_FOUND)
    if(LOOM_LIBFABRIC_STATIC)
        pkg_check_modules(LIBFABRIC IMPORTED_TARGET libfabric)
        if(LIBFABRIC_FOUND)
            # For static linking, we need the static library
            pkg_check_modules(LIBFABRIC_STATIC IMPORTED_TARGET STATIC libfabric)
            if(LIBFABRIC_STATIC_FOUND)
                set(LOOM_LIBFABRIC_TARGET PkgConfig::LIBFABRIC_STATIC)
                set(LOOM_LIBFABRIC_FOUND ON)
                message(STATUS "Using system libfabric (static): ${LIBFABRIC_STATIC_VERSION}")
            endif()
        endif()
    else()
        pkg_check_modules(LIBFABRIC IMPORTED_TARGET libfabric)
        if(LIBFABRIC_FOUND)
            set(LOOM_LIBFABRIC_TARGET PkgConfig::LIBFABRIC)
            set(LOOM_LIBFABRIC_FOUND ON)
            message(STATUS "Using system libfabric (shared): ${LIBFABRIC_VERSION}")
        endif()
    endif()
endif()

# Fall back to third_party if not found
if(NOT LOOM_LIBFABRIC_FOUND)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libfabric/CMakeLists.txt")
        message(STATUS "Using libfabric from third_party")
        set(LIBFABRIC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        add_subdirectory(third_party/libfabric EXCLUDE_FROM_ALL)
        set(LOOM_LIBFABRIC_TARGET fabric)
        set(LOOM_LIBFABRIC_FOUND ON)
    else()
        message(FATAL_ERROR
            "libfabric not found. Please either:\n"
            "  1. Install libfabric (apt install libfabric-dev / dnf install libfabric-devel)\n"
            "  2. Provide libfabric in third_party/libfabric\n"
            "  3. Set PKG_CONFIG_PATH to include libfabric's pkgconfig directory")
    endif()
endif()

# -----------------------------------------------------------------------------
# stdexec (optional)
# -----------------------------------------------------------------------------

set(LOOM_HAS_STDEXEC OFF)
set(LOOM_STDEXEC_TARGET "")

if(LOOM_USE_STDEXEC)
    find_package(stdexec QUIET)

    if(stdexec_FOUND)
        message(STATUS "Found stdexec via find_package")
        set(LOOM_HAS_STDEXEC ON)
        set(LOOM_STDEXEC_TARGET stdexec::stdexec)
    elseif(LOOM_FETCHCONTENT_STDEXEC)
        include(FetchContent)
        message(STATUS "Fetching stdexec via FetchContent")

        FetchContent_Declare(
            stdexec
            GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
            GIT_TAG        250f35737790392d666fd157e0af9be16d0c789f
            GIT_SHALLOW    TRUE
        )

        FetchContent_MakeAvailable(stdexec)
        set(LOOM_HAS_STDEXEC ON)
        # stdexec's CMake exports STDEXEC::stdexec when built via FetchContent
        set(LOOM_STDEXEC_TARGET STDEXEC::stdexec)

        # Mark stdexec includes as SYSTEM to suppress warnings from its headers
        if(TARGET stdexec)
            get_target_property(STDEXEC_INCLUDE_DIRS stdexec INTERFACE_INCLUDE_DIRECTORIES)
            if(STDEXEC_INCLUDE_DIRS)
                set_target_properties(stdexec PROPERTIES
                    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${STDEXEC_INCLUDE_DIRS}"
                )
            endif()
        endif()
    else()
        message(STATUS "stdexec not found and FetchContent disabled")
    endif()
endif()

# -----------------------------------------------------------------------------
# Asio (optional)
# -----------------------------------------------------------------------------

set(LOOM_HAS_ASIO OFF)
set(LOOM_ASIO_FETCHCONTENT OFF)

if(LOOM_USE_ASIO)
    find_package(asio QUIET)

    if(asio_FOUND)
        message(STATUS "Found Asio via find_package")
        set(LOOM_HAS_ASIO ON)
        set(LOOM_ASIO_TARGET asio::asio)
    elseif(PKG_CONFIG_FOUND)
        pkg_check_modules(ASIO QUIET IMPORTED_TARGET asio)
        if(ASIO_FOUND)
            message(STATUS "Found Asio via pkg-config")
            set(LOOM_HAS_ASIO ON)
            set(LOOM_ASIO_TARGET PkgConfig::ASIO)
        endif()
    endif()

    if(NOT LOOM_HAS_ASIO AND LOOM_FETCHCONTENT_ASIO)
        include(FetchContent)
        message(STATUS "Fetching Asio via FetchContent")

        FetchContent_Declare(
            asio
            GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
            GIT_TAG        55684d42ac00021b4c31ba8571aca414c863ead5
            GIT_SHALLOW    TRUE
        )

        FetchContent_MakeAvailable(asio)

        # Create interface library for header-only asio
        # Note: This target is not exportable as it uses build-tree paths
        add_library(loom_asio_headers INTERFACE)
        # Mark as SYSTEM to suppress warnings from asio headers
        target_include_directories(loom_asio_headers SYSTEM INTERFACE
            ${asio_SOURCE_DIR}/asio/include
        )
        target_compile_definitions(loom_asio_headers INTERFACE
            ASIO_STANDALONE
            ASIO_NO_DEPRECATED
        )
        set(LOOM_ASIO_TARGET loom_asio_headers)
        set(LOOM_HAS_ASIO ON)
        set(LOOM_ASIO_FETCHCONTENT ON)
    endif()

    if(NOT LOOM_HAS_ASIO)
        message(STATUS "Asio not found - Asio integration will be skipped")
    endif()
endif()

# -----------------------------------------------------------------------------
# beman::execution (optional)
# -----------------------------------------------------------------------------

set(LOOM_HAS_BEMAN_EXECUTION OFF)

if(LOOM_USE_BEMAN_EXECUTION)
    find_package(beman_execution QUIET)

    if(beman_execution_FOUND)
        message(STATUS "Found beman::execution via find_package")
        set(LOOM_HAS_BEMAN_EXECUTION ON)
    elseif(LOOM_FETCHCONTENT_BEMAN_EXECUTION)
        include(FetchContent)
        message(STATUS "Fetching beman::execution via FetchContent")

        FetchContent_Declare(
            beman_execution
            GIT_REPOSITORY https://github.com/beman-project/execution26.git
            GIT_TAG        6a0bc94ec2497ad04289f24d0a57501d447afa8b
            GIT_SHALLOW    TRUE
        )

        FetchContent_MakeAvailable(beman_execution)
        set(LOOM_HAS_BEMAN_EXECUTION ON)
    else()
        message(STATUS "beman::execution not found and FetchContent disabled")
    endif()
endif()

# =============================================================================
# Main library: loom
# =============================================================================

add_library(loom)
add_library(loom::loom ALIAS loom)

# Set library properties
set_target_properties(loom PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME loom
    OUTPUT_NAME loom
)

# Source files
target_sources(loom PRIVATE
    src/address.cpp
    src/address_vector.cpp
    src/atomic.cpp
    src/collective.cpp
    src/completion_queue.cpp
    src/counter.cpp
    src/domain.cpp
    src/endpoint.cpp
    src/endpoint_bind.cpp
    src/endpoint_options.cpp
    src/error.cpp
    src/event_queue.cpp
    src/fabric.cpp
    src/memory.cpp
    src/passive_endpoint.cpp
    src/rma.cpp
    src/scalable_endpoint.cpp
    src/shared_context.cpp
    src/trigger.cpp
)

# Public headers using FILE_SET (CMake 3.23+)
target_sources(loom PUBLIC
    FILE_SET HEADERS
    TYPE HEADERS
    BASE_DIRS include
    FILES
        # Top-level headers
        include/loom/loom.hpp
        include/loom/async.hpp
        include/loom/asio.hpp
        include/loom/execution.hpp
        include/loom/fabric_query.hpp
        include/loom/provider_range.hpp
        include/loom/cxx_bridge.hpp

        # Core headers
        include/loom/core/address.hpp
        include/loom/core/address_vector.hpp
        include/loom/core/allocator.hpp
        include/loom/core/atomic.hpp
        include/loom/core/collective.hpp
        include/loom/core/concepts.hpp
        include/loom/core/counter.hpp
        include/loom/core/domain.hpp
        include/loom/core/endpoint.hpp
        include/loom/core/endpoint_options.hpp
        include/loom/core/endpoint_types.hpp
        include/loom/core/error.hpp
        include/loom/core/event_queue.hpp
        include/loom/core/fabric.hpp
        include/loom/core/formatters.hpp
        include/loom/core/immediate_data.hpp
        include/loom/core/memory.hpp
        include/loom/core/messaging.hpp
        include/loom/core/mr_cache.hpp
        include/loom/core/passive_endpoint.hpp
        include/loom/core/progress_policy.hpp
        include/loom/core/provider_atomic.hpp
        include/loom/core/provider_aware.hpp
        include/loom/core/request_context.hpp
        include/loom/core/result.hpp
        include/loom/core/rma.hpp
        include/loom/core/safe_context.hpp
        include/loom/core/scalable_endpoint.hpp
        include/loom/core/shared_context.hpp
        include/loom/core/submission_context.hpp
        include/loom/core/trigger.hpp
        include/loom/core/types.hpp

        # Core concepts headers
        include/loom/core/concepts/address.hpp
        include/loom/core/concepts/address_vector.hpp
        include/loom/core/concepts/atomic.hpp
        include/loom/core/concepts/callback.hpp
        include/loom/core/concepts/capabilities.hpp
        include/loom/core/concepts/compile_time.hpp
        include/loom/core/concepts/completion_queue.hpp
        include/loom/core/concepts/context.hpp
        include/loom/core/concepts/data.hpp
        include/loom/core/concepts/endpoint.hpp
        include/loom/core/concepts/event.hpp
        include/loom/core/concepts/fabric_info.hpp
        include/loom/core/concepts/fabric_object.hpp
        include/loom/core/concepts/memory.hpp
        include/loom/core/concepts/policy.hpp
        include/loom/core/concepts/protocol_concepts.hpp
        include/loom/core/concepts/provider_traits.hpp
        include/loom/core/concepts/result.hpp
        include/loom/core/concepts/strong_types.hpp

        # CRTP headers
        include/loom/core/crtp/crtp_detection.hpp
        include/loom/core/crtp/crtp_tags.hpp
        include/loom/core/crtp/event_base.hpp
        include/loom/core/crtp/fabric_object_base.hpp
        include/loom/core/crtp/remote_memory_base.hpp

        # Async headers
        include/loom/async/completion_queue.hpp

        # Asio headers
        include/loom/asio/asio_context.hpp
        include/loom/asio/basic_endpoint.hpp
        include/loom/asio/completion_token.hpp
        include/loom/asio/fabric_service.hpp
        include/loom/asio/registered_buffer.hpp

        # Execution headers
        include/loom/execution/concepts.hpp
        include/loom/execution/operation_state.hpp
        include/loom/execution/receiver.hpp
        include/loom/execution/rma.hpp
        include/loom/execution/scheduler.hpp
        include/loom/execution/send_recv.hpp

        # Protocol headers
        include/loom/protocol/protocol.hpp

        # Detail headers
        include/loom/detail/conversions.hpp
)

# Include directories (for non-FILE_SET aware consumers)
target_include_directories(loom
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Link dependencies
target_link_libraries(loom
    PRIVATE
        loom::compile_options
    PUBLIC
        ${LOOM_LIBFABRIC_TARGET}
        Threads::Threads
)

# Compile features
target_compile_features(loom PUBLIC cxx_std_23)

# Define LOOM_SHARED when building shared library (for potential dllexport/dllimport)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(loom
        PUBLIC LOOM_SHARED
        PRIVATE LOOM_BUILDING_SHARED
    )
endif()

# =============================================================================
# Optional library: loom::execution (stdexec integration)
# =============================================================================

if(LOOM_HAS_STDEXEC)
    add_library(loom_execution INTERFACE)
    add_library(loom::execution ALIAS loom_execution)

    target_link_libraries(loom_execution
        INTERFACE
            loom::loom
            ${LOOM_STDEXEC_TARGET}
    )

    target_compile_definitions(loom_execution
        INTERFACE
            LOOM_HAS_STDEXEC=1
    )

    target_compile_features(loom_execution INTERFACE cxx_std_23)
endif()

# Also support beman::execution as an alternative
if(LOOM_HAS_BEMAN_EXECUTION AND NOT TARGET loom::execution)
    add_library(loom_execution INTERFACE)
    add_library(loom::execution ALIAS loom_execution)

    target_link_libraries(loom_execution
        INTERFACE
            loom::loom
            beman::execution
    )

    target_compile_definitions(loom_execution
        INTERFACE
            LOOM_HAS_BEMAN_EXECUTION=1
    )

    target_compile_features(loom_execution INTERFACE cxx_std_23)
endif()

# =============================================================================
# Optional library: loom::asio (Asio integration)
# =============================================================================

if(LOOM_HAS_ASIO)
    add_library(loom_asio INTERFACE)
    add_library(loom::asio ALIAS loom_asio)

    target_link_libraries(loom_asio
        INTERFACE
            loom::loom
            ${LOOM_ASIO_TARGET}
    )

    target_compile_definitions(loom_asio
        INTERFACE
            LOOM_HAS_ASIO=1
            ASIO_STANDALONE
            ASIO_NO_DEPRECATED
    )

    target_compile_features(loom_asio INTERFACE cxx_std_23)
endif()

# =============================================================================
# Installation
# =============================================================================

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Install the main library
install(TARGETS loom loom_compile_options
    EXPORT loomTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    FILE_SET HEADERS
)

# Install optional libraries if they exist
if(TARGET loom_execution)
    install(TARGETS loom_execution
        EXPORT loomTargets
    )
endif()

# Only install loom_asio if it doesn't depend on FetchContent-provided asio
# (FetchContent targets have build-tree paths that aren't exportable)
if(TARGET loom_asio AND NOT LOOM_ASIO_FETCHCONTENT)
    install(TARGETS loom_asio
        EXPORT loomTargets
    )
endif()

# Install export targets
install(EXPORT loomTargets
    FILE loomTargets.cmake
    NAMESPACE loom::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loom
)

# Generate and install package config files
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/loomConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/loomConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loom
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/loomConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/loomConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/loomConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loom
)

# =============================================================================
# Tests
# =============================================================================

if(LOOM_BUILD_TESTS)
    # Find or fetch Catch2
    find_package(Catch2 3 QUIET)
    if(NOT Catch2_FOUND)
        include(FetchContent)
        message(STATUS "Fetching Catch2 via FetchContent")
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG        v3.7.1
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(Catch2)
        list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    else()
        message(STATUS "Found Catch2 via find_package")
    endif()

    enable_testing()
    add_subdirectory(tests)
endif()

# =============================================================================
# Examples
# =============================================================================

if(LOOM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# =============================================================================
# Rust bindings
# =============================================================================

if(LOOM_BUILD_RUST_BINDINGS)
    find_program(CARGO cargo REQUIRED)

    set(RUST_BINDINGS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bindings/rust")
    set(RUST_TARGET_DIR "${RUST_BINDINGS_DIR}/target")

    # Determine build type for Rust
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(RUST_BUILD_TYPE "debug")
        set(CARGO_BUILD_FLAGS "")
    else()
        set(RUST_BUILD_TYPE "release")
        set(CARGO_BUILD_FLAGS "--release")
    endif()

    add_custom_target(rust_bindings ALL
        COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_DIR=${RUST_TARGET_DIR}"
            ${CARGO} build ${CARGO_BUILD_FLAGS}
        WORKING_DIRECTORY ${RUST_BINDINGS_DIR}
        COMMENT "Building Rust bindings for loom"
        DEPENDS loom
    )

    add_custom_target(rust_bindings_test
        COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_DIR=${RUST_TARGET_DIR}"
            ${CARGO} test ${CARGO_BUILD_FLAGS}
        WORKING_DIRECTORY ${RUST_BINDINGS_DIR}
        COMMENT "Testing Rust bindings for loom"
        DEPENDS rust_bindings
    )
endif()

# =============================================================================
# Coverage targets
# =============================================================================

if(LOOM_ENABLE_COVERAGE AND LOOM_BUILD_TESTS)
    loom_add_coverage_targets()
endif()

# =============================================================================
# Summary
# =============================================================================

message(STATUS "")
message(STATUS "loom ${PROJECT_VERSION} configuration summary:")
message(STATUS "  Build type:        ${CMAKE_BUILD_TYPE}")
message(STATUS "  Shared libs:       ${BUILD_SHARED_LIBS}")
message(STATUS "  libfabric:         ${LOOM_LIBFABRIC_TARGET}")
message(STATUS "  libfabric static:  ${LOOM_LIBFABRIC_STATIC}")
message(STATUS "  stdexec:           ${LOOM_HAS_STDEXEC}")
message(STATUS "  Asio:              ${LOOM_HAS_ASIO}")
message(STATUS "  beman::execution:  ${LOOM_HAS_BEMAN_EXECUTION}")
message(STATUS "  Tests:             ${LOOM_BUILD_TESTS}")
message(STATUS "  Examples:          ${LOOM_BUILD_EXAMPLES}")
message(STATUS "  Rust bindings:     ${LOOM_BUILD_RUST_BINDINGS}")
message(STATUS "  IWYU:              ${LOOM_ENABLE_IWYU}")
message(STATUS "  Coverage:          ${LOOM_ENABLE_COVERAGE}")
message(STATUS "")
