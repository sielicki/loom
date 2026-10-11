cmake_minimum_required(VERSION 3.30)
project(loom_nccl LANGUAGES CXX)

find_package(loom CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NCCL REQUIRED IMPORTED_TARGET nccl-net)
find_package(Threads REQUIRED)
include_directories(../hmem)

add_library(nccl-net-loom SHARED plugin.cpp)
target_compile_features(nccl-net-loom PRIVATE cxx_std_26)
target_compile_options(nccl-net-loom PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(nccl-net-loom PRIVATE loom::loom PkgConfig::NCCL Threads::Threads ${CMAKE_DL_LIBS})

add_executable(nccl-loom-workload workload.cpp)
target_compile_features(nccl-loom-workload PRIVATE cxx_std_26)
target_compile_options(nccl-loom-workload PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(nccl-loom-workload PRIVATE PkgConfig::NCCL ${CMAKE_DL_LIBS})

add_executable(nccl-cuda-policy-check ../hmem/test-cuda.cpp)
target_compile_features(nccl-cuda-policy-check PRIVATE cxx_std_26)
target_compile_options(nccl-cuda-policy-check PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(nccl-cuda-policy-check PRIVATE PkgConfig::NCCL ${CMAKE_DL_LIBS})

add_executable(nccl-loom-cuda-workload gpu-workload.cpp)
target_compile_features(nccl-loom-cuda-workload PRIVATE cxx_std_26)
target_compile_options(nccl-loom-cuda-workload PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(nccl-loom-cuda-workload PRIVATE PkgConfig::NCCL ${CMAKE_DL_LIBS})

# Assemble in CPU-only CI to validate PTX syntax. The workload loads portable
# PTX for the runner's GPU; this cubin is only a build check.
find_program(CUDA_PTXAS ptxas REQUIRED)
add_custom_command(OUTPUT visibility.cubin
    COMMAND "${CUDA_PTXAS}" -arch=sm_70 "${CMAKE_CURRENT_SOURCE_DIR}/visibility.ptx" -o visibility.cubin
    DEPENDS visibility.ptx VERBATIM)
add_custom_target(nccl-cuda-kernel-check ALL DEPENDS visibility.cubin)

enable_testing()
add_test(NAME nccl_cuda_policy COMMAND nccl-cuda-policy-check)
foreach(provider IN ITEMS sockets "tcp;ofi_rxm")
    string(REPLACE ";" "_" profile "${provider}")
    add_test(NAME nccl_${profile} COMMAND nccl-loom-workload $<TARGET_FILE:nccl-net-loom> "${provider}")
    add_test(NAME nccl_process_${profile} COMMAND nccl-loom-workload $<TARGET_FILE:nccl-net-loom> "${provider}" --processes)
    set_tests_properties(nccl_${profile} nccl_process_${profile} PROPERTIES TIMEOUT 30)
endforeach()

install(TARGETS nccl-net-loom nccl-loom-workload nccl-loom-cuda-workload LIBRARY DESTINATION lib RUNTIME DESTINATION bin)

install(FILES visibility.ptx DESTINATION share/nccl-loom)
