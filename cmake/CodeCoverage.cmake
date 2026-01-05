# CodeCoverage.cmake - Code coverage support for loom
#
# This module provides code coverage support using llvm-cov/gcov and
# generates reports in multiple formats (HTML, text, lcov).
#
# Usage:
#   cmake -DLOOM_ENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug ..
#   cmake --build .
#   ctest
#   cmake --build . --target coverage-report
#
# Options:
#   LOOM_ENABLE_COVERAGE     - Enable coverage instrumentation (default: OFF)
#   LOOM_COVERAGE_FORMAT     - Report format: html, text, lcov (default: html)
#
# Targets:
#   coverage-report  - Generate coverage report after running tests
#   coverage-clean   - Clean coverage data files

include_guard(GLOBAL)

# Check if coverage is enabled
option(LOOM_ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
set(LOOM_COVERAGE_FORMAT "html" CACHE STRING "Coverage report format (html, text, lcov)")
set_property(CACHE LOOM_COVERAGE_FORMAT PROPERTY STRINGS html text lcov)

if(NOT LOOM_ENABLE_COVERAGE)
    return()
endif()

# Coverage requires Debug build type for meaningful results
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(WARNING "Code coverage results are more accurate with CMAKE_BUILD_TYPE=Debug")
endif()

# Detect compiler and set appropriate coverage flags
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # LLVM/Clang coverage using source-based coverage (more accurate)
    set(LOOM_COVERAGE_COMPILE_FLAGS
        -fprofile-instr-generate
        -fcoverage-mapping
        -fno-inline
        -O0
    )
    set(LOOM_COVERAGE_LINK_FLAGS
        -fprofile-instr-generate
        -fcoverage-mapping
    )
    set(LOOM_COVERAGE_TOOL "llvm-cov")

    # Find llvm-cov and llvm-profdata
    # Get the compiler directory to look for sibling tools
    get_filename_component(CLANG_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)

    find_program(LLVM_COV_PATH
        NAMES llvm-cov llvm-cov-21 llvm-cov-20 llvm-cov-19 llvm-cov-18 llvm-cov-17
        HINTS
            "${CLANG_BIN_DIR}"
            "${CLANG_BIN_DIR}/../lib/llvm/bin"
            ${LLVM_TOOLS_BINARY_DIR}
            ENV LLVM_DIR
    )
    find_program(LLVM_PROFDATA_PATH
        NAMES llvm-profdata llvm-profdata-21 llvm-profdata-20 llvm-profdata-19 llvm-profdata-18 llvm-profdata-17
        HINTS
            "${CLANG_BIN_DIR}"
            "${CLANG_BIN_DIR}/../lib/llvm/bin"
            ${LLVM_TOOLS_BINARY_DIR}
            ENV LLVM_DIR
    )

    if(NOT LLVM_COV_PATH OR NOT LLVM_PROFDATA_PATH)
        message(FATAL_ERROR
            "Code coverage requires llvm-cov and llvm-profdata.\n"
            "Please ensure LLVM tools are installed and in PATH.\n"
            "On Nix, use: nix develop .#coverage\n"
            "Set LLVM_TOOLS_BINARY_DIR to the LLVM bin directory if needed.\n"
            "Compiler is at: ${CMAKE_CXX_COMPILER}")
    endif()

    message(STATUS "Using LLVM coverage tools:")
    message(STATUS "  llvm-cov: ${LLVM_COV_PATH}")
    message(STATUS "  llvm-profdata: ${LLVM_PROFDATA_PATH}")

elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    # GCC coverage using gcov
    set(LOOM_COVERAGE_COMPILE_FLAGS
        --coverage
        -fprofile-arcs
        -ftest-coverage
        -fno-inline
        -O0
    )
    set(LOOM_COVERAGE_LINK_FLAGS
        --coverage
    )
    set(LOOM_COVERAGE_TOOL "gcov")

    # Find gcov and lcov/genhtml for report generation
    find_program(GCOV_PATH gcov REQUIRED)
    find_program(LCOV_PATH lcov)
    find_program(GENHTML_PATH genhtml)

    message(STATUS "Using GCC coverage tools:")
    message(STATUS "  gcov: ${GCOV_PATH}")
    if(LCOV_PATH)
        message(STATUS "  lcov: ${LCOV_PATH}")
    endif()
    if(GENHTML_PATH)
        message(STATUS "  genhtml: ${GENHTML_PATH}")
    endif()

else()
    message(FATAL_ERROR "Code coverage not supported for compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

# Apply coverage flags globally
add_compile_options(${LOOM_COVERAGE_COMPILE_FLAGS})
add_link_options(${LOOM_COVERAGE_LINK_FLAGS})

message(STATUS "Code coverage enabled with ${LOOM_COVERAGE_TOOL}")

# Coverage output directories
set(LOOM_COVERAGE_DIR "${CMAKE_BINARY_DIR}/coverage")
set(LOOM_COVERAGE_PROFRAW_DIR "${CMAKE_BINARY_DIR}/coverage-profraw")
set(LOOM_COVERAGE_REPORT_DIR "${CMAKE_BINARY_DIR}/coverage-report")

# Create directories
file(MAKE_DIRECTORY ${LOOM_COVERAGE_DIR})
file(MAKE_DIRECTORY ${LOOM_COVERAGE_PROFRAW_DIR})
file(MAKE_DIRECTORY ${LOOM_COVERAGE_REPORT_DIR})

# Set environment variable for llvm profile output
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(ENV{LLVM_PROFILE_FILE} "${LOOM_COVERAGE_PROFRAW_DIR}/%p-%m.profraw")
endif()

# Function to setup coverage for a target
function(loom_enable_coverage_for_target TARGET)
    if(NOT LOOM_ENABLE_COVERAGE)
        return()
    endif()

    target_compile_options(${TARGET} PRIVATE ${LOOM_COVERAGE_COMPILE_FLAGS})
    target_link_options(${TARGET} PRIVATE ${LOOM_COVERAGE_LINK_FLAGS})
endfunction()

# Function to create coverage report target
function(loom_add_coverage_targets)
    if(NOT LOOM_ENABLE_COVERAGE)
        return()
    endif()

    # Get all test executables
    get_property(TEST_TARGETS GLOBAL PROPERTY LOOM_TEST_TARGETS)
    if(NOT TEST_TARGETS)
        # Fallback: collect test executables from tests directory
        file(GLOB TEST_SOURCES "${CMAKE_SOURCE_DIR}/tests/test_*.cpp")
        foreach(SOURCE ${TEST_SOURCES})
            get_filename_component(TEST_NAME ${SOURCE} NAME_WE)
            if(TARGET ${TEST_NAME})
                list(APPEND TEST_TARGETS ${TEST_NAME})
            endif()
        endforeach()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # LLVM coverage report generation
        add_custom_target(coverage-run
            COMMAND ${CMAKE_COMMAND} -E env "LLVM_PROFILE_FILE=${LOOM_COVERAGE_PROFRAW_DIR}/%p-%m.profraw"
                    ${CMAKE_CTEST_COMMAND} --output-on-failure
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Running tests with coverage instrumentation"
        )

        add_custom_target(coverage-merge
            COMMAND ${LLVM_PROFDATA_PATH} merge
                    -sparse
                    -o ${LOOM_COVERAGE_DIR}/merged.profdata
                    ${LOOM_COVERAGE_PROFRAW_DIR}/*.profraw
            DEPENDS coverage-run
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Merging coverage profile data"
        )

        # Build the object file list for llvm-cov
        set(OBJECT_FILES "")
        foreach(TEST ${TEST_TARGETS})
            list(APPEND OBJECT_FILES "-object=$<TARGET_FILE:${TEST}>")
        endforeach()
        list(APPEND OBJECT_FILES "-object=$<TARGET_FILE:loom>")

        if(LOOM_COVERAGE_FORMAT STREQUAL "html")
            add_custom_target(coverage-report
                COMMAND ${LLVM_COV_PATH} show
                        $<TARGET_FILE:loom>
                        ${OBJECT_FILES}
                        -instr-profile=${LOOM_COVERAGE_DIR}/merged.profdata
                        -format=html
                        -output-dir=${LOOM_COVERAGE_REPORT_DIR}
                        -show-line-counts-or-regions
                        -show-instantiations=false
                        -ignore-filename-regex="tests/.*"
                        -ignore-filename-regex="third_party/.*"
                DEPENDS coverage-merge
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Generating HTML coverage report"
            )
        elseif(LOOM_COVERAGE_FORMAT STREQUAL "text")
            add_custom_target(coverage-report
                COMMAND ${LLVM_COV_PATH} report
                        $<TARGET_FILE:loom>
                        ${OBJECT_FILES}
                        -instr-profile=${LOOM_COVERAGE_DIR}/merged.profdata
                        -ignore-filename-regex="tests/.*"
                        -ignore-filename-regex="third_party/.*"
                DEPENDS coverage-merge
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Generating text coverage report"
            )
        elseif(LOOM_COVERAGE_FORMAT STREQUAL "lcov")
            add_custom_target(coverage-report
                COMMAND ${LLVM_COV_PATH} export
                        $<TARGET_FILE:loom>
                        ${OBJECT_FILES}
                        -instr-profile=${LOOM_COVERAGE_DIR}/merged.profdata
                        -format=lcov
                        -ignore-filename-regex="tests/.*"
                        -ignore-filename-regex="third_party/.*"
                        > ${LOOM_COVERAGE_DIR}/coverage.lcov
                DEPENDS coverage-merge
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Generating LCOV coverage report"
            )
        endif()

        # Summary target
        add_custom_target(coverage-summary
            COMMAND ${LLVM_COV_PATH} report
                    $<TARGET_FILE:loom>
                    ${OBJECT_FILES}
                    -instr-profile=${LOOM_COVERAGE_DIR}/merged.profdata
                    -ignore-filename-regex="tests/.*"
                    -ignore-filename-regex="third_party/.*"
            DEPENDS coverage-merge
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Coverage summary"
        )

    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        # GCC/gcov coverage report generation
        if(LCOV_PATH AND GENHTML_PATH)
            add_custom_target(coverage-run
                COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Running tests with coverage instrumentation"
            )

            add_custom_target(coverage-capture
                COMMAND ${LCOV_PATH} --capture
                        --directory ${CMAKE_BINARY_DIR}
                        --output-file ${LOOM_COVERAGE_DIR}/coverage.info
                        --ignore-errors source
                        --rc branch_coverage=1
                DEPENDS coverage-run
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Capturing coverage data"
            )

            add_custom_target(coverage-filter
                COMMAND ${LCOV_PATH} --remove ${LOOM_COVERAGE_DIR}/coverage.info
                        '*/tests/*'
                        '*/third_party/*'
                        '/usr/*'
                        '/nix/*'
                        --output-file ${LOOM_COVERAGE_DIR}/coverage-filtered.info
                        --ignore-errors unused
                        --rc branch_coverage=1
                DEPENDS coverage-capture
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Filtering coverage data"
            )

            if(LOOM_COVERAGE_FORMAT STREQUAL "html")
                add_custom_target(coverage-report
                    COMMAND ${GENHTML_PATH}
                            ${LOOM_COVERAGE_DIR}/coverage-filtered.info
                            --output-directory ${LOOM_COVERAGE_REPORT_DIR}
                            --title "Loom Code Coverage"
                            --legend
                            --show-details
                            --branch-coverage
                    DEPENDS coverage-filter
                    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                    COMMENT "Generating HTML coverage report"
                )
            elseif(LOOM_COVERAGE_FORMAT STREQUAL "lcov")
                add_custom_target(coverage-report
                    COMMAND ${CMAKE_COMMAND} -E copy
                            ${LOOM_COVERAGE_DIR}/coverage-filtered.info
                            ${LOOM_COVERAGE_DIR}/coverage.lcov
                    DEPENDS coverage-filter
                    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                    COMMENT "Generating LCOV coverage report"
                )
            else()
                add_custom_target(coverage-report
                    COMMAND ${LCOV_PATH} --summary
                            ${LOOM_COVERAGE_DIR}/coverage-filtered.info
                            --rc branch_coverage=1
                    DEPENDS coverage-filter
                    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                    COMMENT "Generating text coverage summary"
                )
            endif()

            add_custom_target(coverage-summary
                COMMAND ${LCOV_PATH} --summary
                        ${LOOM_COVERAGE_DIR}/coverage-filtered.info
                        --rc branch_coverage=1
                DEPENDS coverage-filter
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                COMMENT "Coverage summary"
            )
        else()
            message(WARNING "lcov/genhtml not found - coverage reports will be limited")
            add_custom_target(coverage-report
                COMMAND ${CMAKE_COMMAND} -E echo "Install lcov and genhtml for coverage reports"
                COMMENT "Coverage reports require lcov and genhtml"
            )
        endif()
    endif()

    # Clean target
    add_custom_target(coverage-clean
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${LOOM_COVERAGE_DIR}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${LOOM_COVERAGE_PROFRAW_DIR}
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${LOOM_COVERAGE_REPORT_DIR}
        COMMAND find ${CMAKE_BINARY_DIR} -name "*.gcda" -delete 2>/dev/null || true
        COMMAND find ${CMAKE_BINARY_DIR} -name "*.gcno" -delete 2>/dev/null || true
        COMMAND find ${CMAKE_BINARY_DIR} -name "*.profraw" -delete 2>/dev/null || true
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Cleaning coverage data"
    )

    # Print instructions
    message(STATUS "")
    message(STATUS "Coverage targets available:")
    message(STATUS "  coverage-report  - Run tests and generate ${LOOM_COVERAGE_FORMAT} coverage report")
    message(STATUS "  coverage-summary - Run tests and print coverage summary")
    message(STATUS "  coverage-clean   - Clean coverage data")
    if(LOOM_COVERAGE_FORMAT STREQUAL "html")
        message(STATUS "")
        message(STATUS "After running 'cmake --build . --target coverage-report', open:")
        message(STATUS "  ${LOOM_COVERAGE_REPORT_DIR}/index.html")
    endif()
    message(STATUS "")
endfunction()
