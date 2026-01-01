# Core library tests - focus on stability before async/stdexec integration

# Disable unused variable warnings for tests (they're intentional for validation)
add_compile_options(-Wno-unused-variable -Wno-unused-but-set-variable)

add_executable(test_types test_types.cpp)
target_link_libraries(test_types PRIVATE loom::loom)
add_test(NAME test_types COMMAND test_types)

add_executable(test_error test_error.cpp)
target_link_libraries(test_error PRIVATE loom::loom)
add_test(NAME test_error COMMAND test_error)

add_executable(test_memory test_memory.cpp)
target_link_libraries(test_memory PRIVATE loom::loom)
add_test(NAME test_memory COMMAND test_memory)

add_executable(test_mr_cache test_mr_cache.cpp)
target_link_libraries(test_mr_cache PRIVATE loom::loom)
add_test(NAME test_mr_cache COMMAND test_mr_cache)

# Compile-time concept tests using boost::ut
add_executable(test_concepts test_concepts.cpp)
target_include_directories(test_concepts PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_concepts PRIVATE loom::loom)
add_test(NAME test_concepts COMMAND test_concepts)

add_executable(test_rma test_rma.cpp)
target_link_libraries(test_rma PRIVATE loom::loom)
add_test(NAME test_rma COMMAND test_rma)

add_executable(test_atomic test_atomic.cpp)
target_link_libraries(test_atomic PRIVATE loom::loom)
add_test(NAME test_atomic COMMAND test_atomic)

add_executable(test_collective test_collective.cpp)
target_link_libraries(test_collective PRIVATE loom::loom)
add_test(NAME test_collective COMMAND test_collective)

add_executable(test_data_concepts test_data_concepts.cpp)
target_link_libraries(test_data_concepts PRIVATE loom::loom)
add_test(NAME test_data_concepts COMMAND test_data_concepts)

add_executable(test_callback_concepts test_callback_concepts.cpp)
target_link_libraries(test_callback_concepts PRIVATE loom::loom)
add_test(NAME test_callback_concepts COMMAND test_callback_concepts)

add_executable(test_submission_context test_submission_context.cpp)
target_include_directories(test_submission_context PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_submission_context PRIVATE loom::loom)
add_test(NAME test_submission_context COMMAND test_submission_context)

add_executable(test_formatters test_formatters.cpp)
target_link_libraries(test_formatters PRIVATE loom::loom)
add_test(NAME test_formatters COMMAND test_formatters)

add_executable(test_address test_address.cpp)
target_link_libraries(test_address PRIVATE loom::loom)
add_test(NAME test_address COMMAND test_address)

add_executable(test_endpoint_types test_endpoint_types.cpp)
target_link_libraries(test_endpoint_types PRIVATE loom::loom)
add_test(NAME test_endpoint_types COMMAND test_endpoint_types)

add_executable(test_fabric_query test_fabric_query.cpp)
target_link_libraries(test_fabric_query PRIVATE loom::loom)
add_test(NAME test_fabric_query COMMAND test_fabric_query)

add_executable(test_provider_aware test_provider_aware.cpp)
target_link_libraries(test_provider_aware PRIVATE loom::loom)
add_test(NAME test_provider_aware COMMAND test_provider_aware)

add_executable(test_provider_atomic test_provider_atomic.cpp)
target_link_libraries(test_provider_atomic PRIVATE loom::loom)
add_test(NAME test_provider_atomic COMMAND test_provider_atomic)

add_executable(test_trigger test_trigger.cpp)
target_include_directories(test_trigger PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_trigger PRIVATE loom::loom)
add_test(NAME test_trigger COMMAND test_trigger)

# Async/stdexec integration tests - disabled until core is stable
# add_executable(test_async_senders test_async_senders.cpp)
# target_link_libraries(test_async_senders PRIVATE loom::loom)
# add_test(NAME test_async_senders COMMAND test_async_senders)

# Benchmarks - disabled until core is stable
# find_package(MPI QUIET)
# if(MPI_FOUND)
#     add_executable(benchmark_async benchmark_async.cpp)
#     target_link_libraries(benchmark_async PRIVATE loom::loom MPI::MPI_CXX)
#     message(STATUS "MPI-based benchmarks enabled")
# else()
#     message(STATUS "MPI not found - skipping MPI-based benchmarks")
# endif()
#
# add_executable(benchmark_comparison benchmark_comparison.cpp)
# target_link_libraries(benchmark_comparison PRIVATE loom::loom)
#
# # ASIO benchmarks (require asio)
# find_package(asio QUIET)
# if(asio_FOUND)
#     target_link_libraries(benchmark_comparison PRIVATE asio::asio)
#     message(STATUS "ASIO integration enabled for benchmarks")
# endif()

# Fabtests - disabled until core is stable
# add_subdirectory(fabtests)
