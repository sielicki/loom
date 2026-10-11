# SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
# Test configuration for loom

include(Catch)

# =============================================================================
# Test compile options
# =============================================================================

# Create interface library for test-specific compile options
add_library(loom_test_options INTERFACE)

# Disable unused variable warnings for tests (they're intentional for validation)
target_compile_options(loom_test_options INTERFACE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-unused-variable -Wno-unused-but-set-variable>
)

# =============================================================================
# Helper function for adding tests
# =============================================================================

function(loom_add_test TEST_NAME)
    cmake_parse_arguments(ARG
        ""
        "LIBRARY"
        "SOURCES;EXTRA_LIBS"
        ${ARGN}
    )

    # Default to single source file with same name as test
    if(NOT ARG_SOURCES)
        set(ARG_SOURCES ${TEST_NAME}.cpp)
    endif()

    # Default to loom::loom
    if(NOT ARG_LIBRARY)
        set(ARG_LIBRARY loom::loom)
    endif()

    add_executable(${TEST_NAME} ${ARG_SOURCES})

    target_link_libraries(${TEST_NAME}
        PRIVATE
            ${ARG_LIBRARY}
            ${ARG_EXTRA_LIBS}
            loom_test_options
            Catch2::Catch2WithMain
    )

    # Discover tests for CTest
    catch_discover_tests(${TEST_NAME})
endfunction()

# =============================================================================
# Core unit tests
# =============================================================================

loom_add_test(test_types)
loom_add_test(test_error)
loom_add_test(test_memory)
loom_add_test(test_resource_allocations)
loom_add_test(test_mr_cache)
loom_add_test(test_concepts)
loom_add_test(test_rma)
loom_add_test(test_messaging_vectors)
loom_add_test(test_atomic)
loom_add_test(test_collective)
loom_add_test(test_data_concepts)
loom_add_test(test_callback_concepts)
loom_add_test(test_submission_context)
loom_add_test(test_safe_context)
loom_add_test(test_request_context)
loom_add_test(test_completion_queue)
loom_add_test(test_queue_wait)
loom_add_test(test_counter)
loom_add_test(test_empty_resources)
loom_add_test(test_formatters)
loom_add_test(test_address)
loom_add_test(test_address_transport)
loom_add_test(test_endpoint_types)
loom_add_test(test_endpoint_limits)
loom_add_test(test_scalable_endpoint)
loom_add_test(test_endpoint_options)
loom_add_test(test_fabric_query)
loom_add_test(test_fabric_iteration)
loom_add_test(test_provider_aware)
loom_add_test(test_provider_atomic)
loom_add_test(test_trigger)
loom_add_test(test_trigger_transport)

# Exercise implementation mode without linking the compiled loom library.
add_library(loom_test_implementation OBJECT loom_implementation.cpp)
target_include_directories(loom_test_implementation PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(loom_test_implementation
    PRIVATE loom::compile_options
    PUBLIC loom_sanitizers ${LOOM_LIBFABRIC_TARGET} Threads::Threads
)
add_executable(test_implementation
    test_resource_allocations.cpp
    test_empty_resources.cpp
    test_atomic.cpp
    test_address_transport.cpp
    test_completion_queue.cpp
    test_queue_wait.cpp
    test_counter.cpp
    test_scalable_endpoint.cpp
    test_endpoint_limits.cpp
    test_trigger_transport.cpp
    test_fabric_iteration.cpp
)
target_link_libraries(test_implementation PRIVATE
    loom_test_implementation
    loom_test_options
    Catch2::Catch2WithMain
)
catch_discover_tests(test_implementation TEST_PREFIX "implementation: ")

# =============================================================================
# Integration tests (require fabric provider, e.g., sockets/tcp)
# =============================================================================

loom_add_test(test_fabric_integration)

# =============================================================================
# P2300 integration tests
# =============================================================================

if(TARGET loom::execution)
    message(STATUS "Building P2300 integration tests")

    loom_add_test(test_async_senders
        LIBRARY loom::execution
    )
    loom_add_test(test_execution_progress
        LIBRARY loom::execution
    )
else()
    message(STATUS "Skipping P2300 integration tests (no backend selected)")
endif()

# =============================================================================
# Asio integration tests
# =============================================================================

if(LOOM_HAS_ASIO)
    message(STATUS "Building Asio integration tests")

    loom_add_test(test_asio_context
        LIBRARY loom::asio
    )

    loom_add_test(test_asio_registered_buffer
        LIBRARY loom::asio
    )

    loom_add_test(test_asio_fabric_service
        LIBRARY loom::asio
    )

    loom_add_test(test_asio_endpoint
        LIBRARY loom::asio
    )
    loom_add_test(test_asio_vectors
        LIBRARY loom::asio
    )
    loom_add_test(test_asio_cancellation
        LIBRARY loom::asio
    )
else()
    message(STATUS "Skipping Asio integration tests (LOOM_USE_ASIO=OFF)")
endif()
