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
add_executable(hello_test test/hello_test.cpp)
target_link_libraries(hello_test
    PRIVATE
    GTest::gtest
    GTest::gmock
)

add_executable(ref_owner_test test/ref_owner_test.cpp)
target_include_directories(ref_owner_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ref_owner_test
    PRIVATE
    GTest::gtest_main
    GTest::gmock
)

# Enable testing
enable_testing()
add_test(NAME hello_world_runs COMMAND hello_world)
add_test(NAME hello_test COMMAND hello_test)
add_test(NAME ref_owner_test COMMAND ref_owner_test)
