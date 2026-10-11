cmake_minimum_required(VERSION 3.30)
project(loom_nccl LANGUAGES CXX)

find_package(loom CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NCCL REQUIRED IMPORTED_TARGET nccl-net)
find_package(Threads REQUIRED)

add_library(nccl-net-loom SHARED plugin.cpp)
target_compile_features(nccl-net-loom PRIVATE cxx_std_26)
target_compile_options(nccl-net-loom PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(nccl-net-loom PRIVATE loom::loom PkgConfig::NCCL Threads::Threads)

add_executable(nccl-loom-workload workload.cpp)
target_compile_features(nccl-loom-workload PRIVATE cxx_std_26)
target_compile_options(nccl-loom-workload PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(nccl-loom-workload PRIVATE PkgConfig::NCCL ${CMAKE_DL_LIBS})

enable_testing()
foreach(provider IN ITEMS sockets "tcp;ofi_rxm")
    string(REPLACE ";" "_" profile "${provider}")
    add_test(NAME nccl_${profile} COMMAND nccl-loom-workload $<TARGET_FILE:nccl-net-loom> "${provider}")
    add_test(NAME nccl_process_${profile} COMMAND nccl-loom-workload $<TARGET_FILE:nccl-net-loom> "${provider}" --processes)
    set_tests_properties(nccl_${profile} nccl_process_${profile} PROPERTIES TIMEOUT 30)
endforeach()

install(TARGETS nccl-net-loom nccl-loom-workload LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
