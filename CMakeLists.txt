cmake_minimum_required(VERSION 3.25)

project(loom
    VERSION 0.1.0
    LANGUAGES CXX C
    DESCRIPTION "Modern C++23 bindings for libfabric with stdexec/asio integration"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(LOOM_BUILD_TESTS "Build tests" ON)
option(LOOM_BUILD_EXAMPLES "Build examples" OFF)
option(LOOM_BUILD_ZIG_BINDINGS "Build Zig bindings" OFF)
option(LOOM_BUILD_RUST_BINDINGS "Build Rust bindings" OFF)
option(LOOM_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(LOOM_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(
        -Wall -Wextra -Wpedantic -Werror
        -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Woverloaded-virtual
        -Wcast-align -Wunused
    )
endif()

# Sanitizers
if(LOOM_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if(LOOM_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()

# Dependencies
find_package(Threads REQUIRED)

# Try to find system libfabric first
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBFABRIC libfabric)
endif()

# If not found, try to build from source
if(NOT LIBFABRIC_FOUND)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libfabric/CMakeLists.txt")
        message(STATUS "Using libfabric from third_party")
        set(LIBFABRIC_BUILD_TESTS OFF CACHE BOOL "")
        add_subdirectory(third_party/libfabric EXCLUDE_FROM_ALL)
        set(LIBFABRIC_LIBRARIES fabric)
        set(LIBFABRIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libfabric/include")
    else()
        message(FATAL_ERROR "libfabric not found. Please install libfabric or provide it in third_party/")
    endif()
else()
    message(STATUS "Using system libfabric")
endif()

# stdexec - optional dependency for async examples
option(LOOM_USE_STDEXEC "Enable stdexec integration" OFF)
option(LOOM_FETCHCONTENT_STDEXEC "Allow FetchContent for stdexec if not found" OFF)

set(LOOM_HAS_STDEXEC OFF)

if(LOOM_USE_STDEXEC)
    # Try to find stdexec via find_package first (e.g., from Nix)
    find_package(stdexec QUIET)

    if(stdexec_FOUND)
        message(STATUS "Found stdexec via find_package")
        set(LOOM_HAS_STDEXEC ON)
    elseif(LOOM_FETCHCONTENT_STDEXEC)
        # Fall back to FetchContent if allowed
        include(FetchContent)
        message(STATUS "Fetching stdexec via FetchContent")

        FetchContent_Declare(
            stdexec
            GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
            GIT_TAG        a1c78ecfc2cdc38d5f435c688036955d1e18117d
            GIT_SHALLOW    TRUE
        )

        FetchContent_MakeAvailable(stdexec)
        set(LOOM_HAS_STDEXEC ON)
    else()
        message(STATUS "stdexec not found and FetchContent disabled - async examples will be skipped")
    endif()
endif()

# Main library
add_library(loom
    src/address.cpp
    src/error.cpp
    src/fabric.cpp
    src/domain.cpp
    src/endpoint.cpp
    src/endpoint_bind.cpp
    src/endpoint_options.cpp
    src/passive_endpoint.cpp
    src/scalable_endpoint.cpp
    src/memory.cpp
    src/rma.cpp
    src/atomic.cpp
    src/collective.cpp
    src/completion_queue.cpp
    src/event_queue.cpp
    src/address_vector.cpp
    src/counter.cpp
    src/trigger.cpp
    src/shared_context.cpp
)
add_library(loom::loom ALIAS loom)

target_include_directories(loom
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${LIBFABRIC_INCLUDE_DIRS}
)

target_link_libraries(loom
    PUBLIC
        ${LIBFABRIC_LIBRARIES}
        Threads::Threads
)

target_compile_features(loom PUBLIC cxx_std_23)

# Installation
include(GNUInstallDirs)
install(TARGETS loom
    EXPORT loomTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/loom
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT loomTargets
    FILE loomTargets.cmake
    NAMESPACE loom::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loom
)

# Config file
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/loomConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/loomConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/loom
)

# Tests
if(LOOM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
if(LOOM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
