# FindTLC.cmake - Find TLA+ Tools (TLC model checker)
#
# This module defines:
#  TLC_FOUND - System has TLC
#  TLC_JAR - Path to tla2tools.jar
#  TLC_VERSION - Version of TLA+ Tools
#
# If TLC is not found, it will be automatically downloaded from GitHub releases.

# Configuration: TLA+ tools version
set(TLC_VERSION "1.8.0" CACHE STRING "TLA+ Tools version to download if not found")
set(TLC_URL "https://github.com/tlaplus/tlaplus/releases/download/v${TLC_VERSION}/tla2tools.jar")
set(TLC_INSTALL_DIR "${CMAKE_SOURCE_DIR}/external/tla2tools-${TLC_VERSION}")

# First, try to find TLC in standard locations
find_file(TLC_JAR
    NAMES tla2tools.jar
    PATHS
        /usr/local/lib
        /opt/homebrew/lib
        $ENV{HOME}/.local/lib
        ${CMAKE_SOURCE_DIR}/tools
        ${TLC_INSTALL_DIR}
    PATH_SUFFIXES tla2tools
    DOC "Path to TLA+ Tools jar file (tla2tools.jar)"
    NO_DEFAULT_PATH
)

# If not found, try default paths
if(NOT TLC_JAR)
    find_file(TLC_JAR
        NAMES tla2tools.jar
        PATH_SUFFIXES tla2tools
        DOC "Path to TLA+ Tools jar file (tla2tools.jar)"
    )
endif()

# If still not found, download from GitHub releases
if(NOT TLC_JAR)
    message(STATUS "TLA+ Tools not found. Downloading version ${TLC_VERSION}...")
    
    file(MAKE_DIRECTORY ${TLC_INSTALL_DIR})
    set(TLC_JAR "${TLC_INSTALL_DIR}/tla2tools.jar")
    
    # Download the jar file
    if(NOT EXISTS ${TLC_JAR})
        file(DOWNLOAD
            ${TLC_URL}
            ${TLC_JAR}
            STATUS DOWNLOAD_STATUS
            SHOW_PROGRESS
        )
        
        list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
        list(GET DOWNLOAD_STATUS 1 ERROR_MESSAGE)
        
        if(NOT STATUS_CODE EQUAL 0)
            message(FATAL_ERROR "Failed to download TLA+ Tools: ${ERROR_MESSAGE}")
        endif()
        
        message(STATUS "Downloaded TLA+ Tools ${TLC_VERSION}")
    else()
        message(STATUS "Using cached TLA+ Tools")
    endif()
    
    message(STATUS "Using downloaded TLA+ Tools: ${TLC_JAR}")
endif()

# Extract version if jar is found
if(TLC_JAR)
    execute_process(
        COMMAND java -jar ${TLC_JAR} -h
        OUTPUT_VARIABLE TLC_VERSION_OUTPUT
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    
    if(TLC_VERSION_OUTPUT MATCHES "Version ([0-9]+\\.[0-9]+\\.[0-9]+)")
        set(TLC_VERSION ${CMAKE_MATCH_1})
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(TLC
    REQUIRED_VARS TLC_JAR
    VERSION_VAR TLC_VERSION
)

mark_as_advanced(TLC_JAR TLC_VERSION)

# Create imported target
if(TLC_FOUND AND NOT TARGET TLC::TLC)
    add_executable(TLC::TLC IMPORTED)
    set_target_properties(TLC::TLC PROPERTIES
        IMPORTED_LOCATION ${TLC_JAR}
    )
endif()
