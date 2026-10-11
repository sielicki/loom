cmake_minimum_required(VERSION 3.30)
project(loom_nixl LANGUAGES CXX)

find_package(loom CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NIXL REQUIRED IMPORTED_TARGET nixl)
find_package(Threads REQUIRED)

add_library(nixl_loom MODULE backend.cpp)
set_target_properties(nixl_loom PROPERTIES OUTPUT_NAME plugin_LOOM)
target_compile_features(nixl_loom PRIVATE cxx_std_26)
target_link_libraries(nixl_loom PRIVATE loom::loom PkgConfig::NIXL Threads::Threads)
target_compile_options(nixl_loom PRIVATE -Wall -Wextra -Wpedantic)

add_executable(nixl-loom-workload workload.cpp)
target_compile_features(nixl-loom-workload PRIVATE cxx_std_26)
target_link_libraries(nixl-loom-workload PRIVATE PkgConfig::NIXL Threads::Threads)

add_executable(nixl-contract-check test-contract.cpp)
target_compile_features(nixl-contract-check PRIVATE cxx_std_26)
target_compile_options(nixl-contract-check PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(nixl-contract-check PRIVATE PkgConfig::NIXL Threads::Threads ${CMAKE_DL_LIBS})

enable_testing()
foreach(provider IN ITEMS sockets "tcp;ofi_rxm")
    string(REPLACE ";" "_" profile "${provider}")
    add_test(NAME nixl_contract_${profile}
             COMMAND nixl-contract-check $<TARGET_FILE:nixl_loom> "${provider}")
    set_tests_properties(nixl_contract_${profile} PROPERTIES TIMEOUT 30)
    add_test(NAME nixl_${profile}
             COMMAND nixl-loom-workload "${provider}")
    add_test(NAME nixl_process_${profile}
             COMMAND nixl-loom-workload "${provider}" --processes)
    set_tests_properties(nixl_${profile} nixl_process_${profile} PROPERTIES
        ENVIRONMENT "NIXL_PLUGIN_DIR=$<TARGET_FILE_DIR:nixl_loom>"
        TIMEOUT 30)
endforeach()

install(TARGETS nixl_loom LIBRARY DESTINATION lib/nixl)
install(TARGETS nixl-loom-workload RUNTIME DESTINATION bin)
