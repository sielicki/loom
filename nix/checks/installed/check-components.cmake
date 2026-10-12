# SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
cmake_minimum_required(VERSION 3.30)

if(NOT DEFINED LOOM_CHECK_DIR)
    set(LOOM_CHECK_DIR "${CMAKE_CURRENT_BINARY_DIR}/component-checks")
endif()
set(consumer_source "${CMAKE_CURRENT_LIST_DIR}")
set(disable_frontends
    -DCMAKE_DISABLE_FIND_PACKAGE_stdexec=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_beman.execution=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_asio=ON)

function(check_consumer name expected_error)
    set(build "${LOOM_CHECK_DIR}/${name}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --fresh -S "${consumer_source}" -B "${build}"
            "-DCMAKE_PREFIX_PATH=${LOOM_CHECK_PREFIX_PATH}"
            ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(expected_error)
        if(result EQUAL 0 OR NOT "${output}\n${error}" MATCHES "${expected_error}")
            message(FATAL_ERROR "${name}: expected ${expected_error}\n${output}\n${error}")
        endif()
        message(STATUS "Installed consumer: ${name} rejected as expected")
        return()
    elseif(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: configure failed\n${output}\n${error}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --parallel 2
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: build failed\n${output}\n${error}")
    endif()
    execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${build}" --output-on-failure
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: execution failed\n${output}\n${error}")
    endif()
    message(STATUS "Installed consumer: ${name} passed")
endfunction()

check_consumer(default "")
include("${LOOM_CHECK_DIR}/default/components.cmake")
set(available core)
if(has_execution)
    list(APPEND available execution)
    check_consumer(execution "" -DCHECK_COMPONENTS=execution -DCHECK_EXPECT_asio=OFF)
endif()
if(has_asio)
    list(APPEND available asio)
    check_consumer(asio "" -DCHECK_COMPONENTS=asio -DCHECK_EXPECT_execution=OFF)
endif()
check_consumer(repeated "" "-DCHECK_COMPONENTS=${available}" -DCHECK_REPEAT_FIND=ON)
check_consumer(core "" -DCHECK_COMPONENTS=core
    -DCHECK_EXPECT_execution=OFF -DCHECK_EXPECT_asio=OFF)
check_consumer(core_without_frontends "" -DCHECK_COMPONENTS=core ${disable_frontends}
    -DCHECK_EXPECT_execution=OFF -DCHECK_EXPECT_asio=OFF)
check_consumer(default_without_frontends "" ${disable_frontends} -DCHECK_EXPECT_asio=OFF)
include("${LOOM_CHECK_DIR}/default_without_frontends/components.cmake")
if(NOT has_execution)
    check_consumer(missing_execution "loom::execution" ${disable_frontends}
        -DCHECK_COMPONENTS=execution)
endif()
check_consumer(missing_asio "loom::asio" ${disable_frontends} -DCHECK_COMPONENTS=asio)
check_consumer(optional_frontends "" ${disable_frontends} -DCHECK_COMPONENTS=core
    "-DCHECK_OPTIONAL_COMPONENTS=execution;asio;unknown" -DCHECK_EXPECT_asio=OFF)
check_consumer(unknown_component "Unknown loom component" -DCHECK_COMPONENTS=unknown)
