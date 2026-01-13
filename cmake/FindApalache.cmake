# FindApalache.cmake - Find Apalache symbolic model checker
#
# This module defines:
#  Apalache_FOUND - System has Apalache
#  APALACHE_EXECUTABLE - Path to apalache-mc
#  Apalache_VERSION - Version of Apalache
#
# If Apalache is not found, it will be automatically downloaded from GitHub releases.

# Configuration: Apalache version and checksums
set(APALACHE_VERSION "0.52.1" CACHE STRING "Apalache version to download if not found")
set(APALACHE_URL "https://github.com/apalache-mc/apalache/releases/download/v${APALACHE_VERSION}/apalache-${APALACHE_VERSION}.tgz")
set(APALACHE_SHA256 "c539711703fd2550d8e065e486f0cbc8286846e14c16e92ef93ba3ece0149ef3" CACHE STRING "SHA256 checksum for Apalache ${APALACHE_VERSION}")
set(APALACHE_INSTALL_DIR "${CMAKE_SOURCE_DIR}/external/apalache-${APALACHE_VERSION}")

# First, try to find Apalache in standard locations
find_program(APALACHE_EXECUTABLE
    NAMES apalache-mc
    PATHS
        /usr/local/bin
        /opt/homebrew/bin
        $ENV{HOME}/.local/bin
        ${CMAKE_SOURCE_DIR}/tools
        ${APALACHE_INSTALL_DIR}
    PATH_SUFFIXES apalache bin
    DOC "Path to Apalache model checker"
    NO_DEFAULT_PATH
)

# If not found, try default paths
if(NOT APALACHE_EXECUTABLE)
    find_program(APALACHE_EXECUTABLE
        NAMES apalache-mc
        PATH_SUFFIXES apalache bin
        DOC "Path to Apalache model checker"
    )
endif()

# If still not found, download and extract from GitHub releases
if(NOT APALACHE_EXECUTABLE)
    message(STATUS "Apalache not found. Downloading version ${APALACHE_VERSION}...")
    
    set(APALACHE_ARCHIVE "${CMAKE_SOURCE_DIR}/external/apalache-${APALACHE_VERSION}.tgz")
    
    # Download the archive
    if(NOT EXISTS ${APALACHE_ARCHIVE})
        file(DOWNLOAD
            ${APALACHE_URL}
            ${APALACHE_ARCHIVE}
            EXPECTED_HASH SHA256=${APALACHE_SHA256}
            STATUS DOWNLOAD_STATUS
            SHOW_PROGRESS
        )
        
        list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
        list(GET DOWNLOAD_STATUS 1 ERROR_MESSAGE)
        
        if(NOT STATUS_CODE EQUAL 0)
            message(FATAL_ERROR "Failed to download Apalache: ${ERROR_MESSAGE}")
        endif()
        
        message(STATUS "Downloaded Apalache ${APALACHE_VERSION}")
    else()
        message(STATUS "Using cached Apalache archive")
    endif()
    
    # Extract the archive
    if(NOT EXISTS ${APALACHE_INSTALL_DIR})
        message(STATUS "Extracting Apalache...")
        file(MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/external)
        
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xzf ${APALACHE_ARCHIVE}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/external
            RESULT_VARIABLE EXTRACT_RESULT
        )
        
        if(NOT EXTRACT_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to extract Apalache archive")
        endif()
        
        message(STATUS "Extracted Apalache to ${APALACHE_INSTALL_DIR}")
    endif()
    
    # Find the executable in the extracted directory
    find_program(APALACHE_EXECUTABLE
        NAMES apalache-mc
        PATHS ${APALACHE_INSTALL_DIR}
        PATH_SUFFIXES bin
        NO_DEFAULT_PATH
        REQUIRED
    )
    
    if(APALACHE_EXECUTABLE)
        message(STATUS "Using downloaded Apalache: ${APALACHE_EXECUTABLE}")
    endif()
endif()

# Extract version
if(APALACHE_EXECUTABLE)
    execute_process(
        COMMAND ${APALACHE_EXECUTABLE} version
        OUTPUT_VARIABLE APALACHE_VERSION_OUTPUT
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    
    if(APALACHE_VERSION_OUTPUT MATCHES "version ([0-9]+\\.[0-9]+\\.[0-9]+)")
        set(Apalache_VERSION ${CMAKE_MATCH_1})
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Apalache
    REQUIRED_VARS APALACHE_EXECUTABLE
    VERSION_VAR Apalache_VERSION
)

mark_as_advanced(APALACHE_EXECUTABLE Apalache_VERSION)

# Create imported target
if(Apalache_FOUND AND NOT TARGET Apalache::apalache)
    add_executable(Apalache::apalache IMPORTED)
    set_target_properties(Apalache::apalache PROPERTIES
        IMPORTED_LOCATION ${APALACHE_EXECUTABLE}
    )
endif()
