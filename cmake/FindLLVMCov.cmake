# FindLLVMCov.cmake - Find LLVM coverage tools and provide coverage target helpers
#
# This module defines:
#  LLVMCov_FOUND - System has LLVM coverage tools
#  LLVM_COV_EXECUTABLE - Path to llvm-cov
#  LLVM_PROFDATA_EXECUTABLE - Path to llvm-profdata
#  LLVMCov_VERSION - Version of llvm-cov
#
# This module provides the following functions:
#  add_coverage_targets(TARGET_SUFFIX EXECUTABLES SOURCE_FILES)
#    - Creates coverage-report-<suffix> and coverage-summary-<suffix> targets
#    - EXECUTABLES: List of executable targets to instrument
#    - SOURCE_FILES: List of source files to include in coverage report

find_program(LLVM_COV_EXECUTABLE
    NAMES llvm-cov
    DOC "Path to llvm-cov for coverage reporting"
)

find_program(LLVM_PROFDATA_EXECUTABLE
    NAMES llvm-profdata
    DOC "Path to llvm-profdata for merging coverage data"
)

# Extract version from llvm-cov
if(LLVM_COV_EXECUTABLE)
    execute_process(
        COMMAND ${LLVM_COV_EXECUTABLE} --version
        OUTPUT_VARIABLE LLVM_COV_VERSION_OUTPUT
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    
    if(LLVM_COV_VERSION_OUTPUT MATCHES "LLVM version ([0-9]+\\.[0-9]+\\.[0-9]+)")
        set(LLVMCov_VERSION ${CMAKE_MATCH_1})
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LLVMCov
    REQUIRED_VARS LLVM_COV_EXECUTABLE LLVM_PROFDATA_EXECUTABLE
    VERSION_VAR LLVMCov_VERSION
)

mark_as_advanced(LLVM_COV_EXECUTABLE LLVM_PROFDATA_EXECUTABLE LLVMCov_VERSION)

# Function to add coverage targets
# Usage: add_coverage_targets(
#   TARGET_SUFFIX <suffix>
#   EXECUTABLES <target1> <target2> ...
#   SOURCE_FILES <file1> <file2> ...
# )
function(add_coverage_targets)
    if(NOT LLVMCov_FOUND)
        message(WARNING "Cannot add coverage targets - LLVM coverage tools not found")
        return()
    endif()
    
    set(options "")
    set(oneValueArgs TARGET_SUFFIX)
    set(multiValueArgs EXECUTABLES SOURCE_FILES)
    cmake_parse_arguments(COV "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    if(NOT COV_TARGET_SUFFIX)
        message(FATAL_ERROR "add_coverage_targets requires TARGET_SUFFIX argument")
    endif()
    
    if(NOT COV_EXECUTABLES)
        message(FATAL_ERROR "add_coverage_targets requires at least one EXECUTABLES argument")
    endif()
    
    # Set paths
    set(COVERAGE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/coverage-${COV_TARGET_SUFFIX}")
    set(PROFRAW_FILE "${CMAKE_BINARY_DIR}/default.profraw")
    set(PROFDATA_FILE "${CMAKE_BINARY_DIR}/merged-${COV_TARGET_SUFFIX}.profdata")
    
    # Build object file list for llvm-cov
    list(GET COV_EXECUTABLES 0 PRIMARY_EXECUTABLE)
    set(OBJECT_FLAGS "")
    list(LENGTH COV_EXECUTABLES EXEC_COUNT)
    if(EXEC_COUNT GREATER 1)
        list(REMOVE_AT COV_EXECUTABLES 0)
        foreach(exec ${COV_EXECUTABLES})
            list(APPEND OBJECT_FLAGS "-object=$<TARGET_FILE:${exec}>")
        endforeach()
    endif()
    
    # HTML coverage report target
    add_custom_target(coverage-report-${COV_TARGET_SUFFIX}
        COMMAND ${CMAKE_COMMAND} -E echo "Generating profdata for ${COV_TARGET_SUFFIX}..."
        COMMAND ${LLVM_PROFDATA_EXECUTABLE} merge -sparse
                ${PROFRAW_FILE}
                -o ${PROFDATA_FILE}
        COMMAND ${CMAKE_COMMAND} -E echo "Generating HTML coverage report for ${COV_TARGET_SUFFIX}..."
        COMMAND ${LLVM_COV_EXECUTABLE} show
                $<TARGET_FILE:${PRIMARY_EXECUTABLE}>
                -instr-profile=${PROFDATA_FILE}
                -format=html
                -output-dir=${COVERAGE_OUTPUT_DIR}
                -show-line-counts-or-regions
                -show-instantiations
                -Xdemangler=c++filt
                ${OBJECT_FLAGS}
                ${COV_SOURCE_FILES}
        COMMAND ${CMAKE_COMMAND} -E echo "Coverage report generated in ${COVERAGE_OUTPUT_DIR}/index.html"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Generating source-based coverage report for ${COV_TARGET_SUFFIX}"
        VERBATIM
    )
    
    # Terminal coverage summary target
    add_custom_target(coverage-summary-${COV_TARGET_SUFFIX}
        COMMAND ${CMAKE_COMMAND} -E echo "Generating profdata for ${COV_TARGET_SUFFIX}..."
        COMMAND ${LLVM_PROFDATA_EXECUTABLE} merge -sparse
                ${PROFRAW_FILE}
                -o ${PROFDATA_FILE}
        COMMAND ${CMAKE_COMMAND} -E echo ""
        COMMAND ${CMAKE_COMMAND} -E echo "Coverage Summary for ${COV_TARGET_SUFFIX}:"
        COMMAND ${CMAKE_COMMAND} -E echo "========================================"
        COMMAND ${LLVM_COV_EXECUTABLE} report
                $<TARGET_FILE:${PRIMARY_EXECUTABLE}>
                -instr-profile=${PROFDATA_FILE}
                ${OBJECT_FLAGS}
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Displaying coverage summary for ${COV_TARGET_SUFFIX}"
        VERBATIM
    )
endfunction()

# Create imported targets if found
if(LLVMCov_FOUND)
    if(NOT TARGET LLVMCov::llvm-cov)
        add_executable(LLVMCov::llvm-cov IMPORTED)
        set_target_properties(LLVMCov::llvm-cov PROPERTIES
            IMPORTED_LOCATION ${LLVM_COV_EXECUTABLE}
        )
    endif()
    
    if(NOT TARGET LLVMCov::llvm-profdata)
        add_executable(LLVMCov::llvm-profdata IMPORTED)
        set_target_properties(LLVMCov::llvm-profdata PROPERTIES
            IMPORTED_LOCATION ${LLVM_PROFDATA_EXECUTABLE}
        )
    endif()
endif()
