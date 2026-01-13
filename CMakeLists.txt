cmake_minimum_required(VERSION 3.14)

# CMake warnings configuration
set(CMAKE_WARN_UNUSED_VARIABLE ON)

project(shared_reference
    VERSION 0.1.0
    LANGUAGES CXX
)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add cmake module path
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# =============================================================================
# Clang-Tidy Integration
# =============================================================================

option(ENABLE_CLANG_TIDY "Enable clang-tidy static analysis" OFF)

if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXECUTABLE
        NAMES clang-tidy
        DOC "Path to clang-tidy executable"
    )
    
    if(CLANG_TIDY_EXECUTABLE)
        message(STATUS "Found clang-tidy: ${CLANG_TIDY_EXECUTABLE}")
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE}")
    else()
        message(WARNING "clang-tidy requested but not found")
    endif()
endif()

# Fetch Google Test
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Compiler warnings
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wunused-variable
)

# Add your source files here
# add_library(shared_reference src/shared_reference.cpp)
# target_include_directories(shared_reference PUBLIC include)

# Example executable
add_executable(hello_world src/main.cpp)

# Tests
add_executable(ref_owner_test test/ref_owner_test.cpp)
target_include_directories(ref_owner_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ref_owner_test
    PRIVATE
    GTest::gtest_main
    GTest::gmock
)

# Enable testing
enable_testing()

# =============================================================================
# TLA+ Verification
# =============================================================================

# Find TLA+ tools
find_package(TLC)
find_package(Apalache)

# TLC verification targets
if(TLC_FOUND)
    add_custom_target(verify-tlc-unique
        COMMAND java -XX:+UseParallelGC -jar ${TLC_JAR}
                -workers auto
                -config UniqueReference.cfg
                UniqueReference.tla
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/spec
        COMMENT "Running TLC model checker on UniqueReference specification"
        VERBATIM
    )
    
    add_custom_target(verify-tlc-shareable
        COMMAND java -XX:+UseParallelGC -jar ${TLC_JAR}
                -workers auto
                -config ShareablePtr.cfg
                ShareablePtr.tla
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/spec
        COMMENT "Running TLC model checker on ShareablePtr specification"
        VERBATIM
    )
    
    add_custom_target(verify-tlc
        DEPENDS verify-tlc-unique verify-tlc-shareable
        COMMENT "Running TLC model checker on all specifications"
    )
    
    message(STATUS "TLC model checker found: ${TLC_JAR}")
    if(TLC_VERSION)
        message(STATUS "  Version: ${TLC_VERSION}")
    endif()
endif()

# Apalache verification targets
if(Apalache_FOUND)
    add_custom_target(verify-apalache-unique
        COMMAND ${APALACHE_EXECUTABLE} check
                --config=UniqueReference.cfg
                UniqueReference.tla
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/spec
        COMMENT "Running Apalache model checker on UniqueReference specification"
        VERBATIM
    )
    
    add_custom_target(verify-apalache-shareable
        COMMAND ${APALACHE_EXECUTABLE} check
                --config=ShareablePtr.cfg
                ShareablePtr.tla
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/spec
        COMMENT "Running Apalache model checker on ShareablePtr specification"
        VERBATIM
    )
    
    add_custom_target(verify-apalache
        DEPENDS verify-apalache-unique verify-apalache-shareable
        COMMENT "Running Apalache model checker on all specifications"
    )
    
    message(STATUS "Apalache model checker found: ${APALACHE_EXECUTABLE}")
    if(Apalache_VERSION)
        message(STATUS "  Version: ${Apalache_VERSION}")
    endif()
endif()

# Combined verification target
if(TLC_FOUND OR Apalache_FOUND)
    add_custom_target(verify-all
        COMMENT "Running all available TLA+ verifications"
    )
    
    if(TLC_FOUND)
        add_dependencies(verify-all verify-tlc)
    endif()
    
    if(Apalache_FOUND)
        add_dependencies(verify-all verify-apalache)
    endif()
    
    message(STATUS "TLA+ verification targets available:")
    if(TLC_FOUND)
        message(STATUS "  - verify-tlc-unique: Verify UniqueReference with TLC")
        message(STATUS "  - verify-tlc-shareable: Verify ShareablePtr with TLC")
        message(STATUS "  - verify-tlc: Verify all specs with TLC")
    endif()
    if(Apalache_FOUND)
        message(STATUS "  - verify-apalache-unique: Verify UniqueReference with Apalache")
        message(STATUS "  - verify-apalache-shareable: Verify ShareablePtr with Apalache")
        message(STATUS "  - verify-apalache: Verify all specs with Apalache")
    endif()
    message(STATUS "  - verify-all: Run all available verifications")
else()
    message(STATUS "No TLA+ model checkers found")
    message(STATUS "  To install TLC: https://github.com/tlaplus/tlaplus/releases")
    message(STATUS "  To install Apalache: https://github.com/apalache-mc/apalache")
endif()
add_test(NAME hello_world_runs COMMAND hello_world)
add_test(NAME ref_owner_test COMMAND ref_owner_test)

# =============================================================================
# Coverage Report Generation (Clang Source-Based Coverage)
# =============================================================================

if(ENABLE_COVERAGE)
    # Find LLVM coverage tools
    find_package(LLVMCov)
    
    if(LLVMCov_FOUND)
        # Create coverage targets for our tests
        add_coverage_targets(
            TARGET_SUFFIX tests
            EXECUTABLES
                ref_owner_test
                hello_world
            SOURCE_FILES
                ${CMAKE_SOURCE_DIR}/include/zoox/memory_w_ref_owner.hpp
                ${CMAKE_SOURCE_DIR}/test/ref_owner_test.cpp
                ${CMAKE_SOURCE_DIR}/src/main.cpp
        )
        
        # Create convenience targets without suffix (aliases to tests)
        add_custom_target(coverage-report
            DEPENDS coverage-report-tests
            COMMENT "Generating coverage report"
        )
        
        add_custom_target(coverage-summary
            DEPENDS coverage-summary-tests
            COMMENT "Displaying coverage summary"
        )
        
        message(STATUS "Coverage reporting enabled")
        message(STATUS "  - Run tests to generate .profraw files")
        message(STATUS "  - Use 'cmake --build . --target coverage-report' to generate HTML report")
        message(STATUS "  - Use 'cmake --build . --target coverage-summary' to see coverage summary")
        message(STATUS "  - llvm-cov: ${LLVM_COV_EXECUTABLE}")
        message(STATUS "  - llvm-profdata: ${LLVM_PROFDATA_EXECUTABLE}")
        if(LLVMCov_VERSION)
            message(STATUS "  - Version: ${LLVMCov_VERSION}")
        endif()
    else()
        message(WARNING "Coverage enabled but LLVM coverage tools not found")
        message(WARNING "  Install llvm-cov and llvm-profdata to enable coverage reporting")
    endif()
endif()
