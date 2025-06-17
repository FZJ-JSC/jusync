cmake_minimum_required(VERSION 3.16)
project(anari_usd_middleware VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------------------
# ZeroMQ Configuration (Portable)
# ------------------------------
find_package(PkgConfig QUIET)

if(WIN32)
    # Try environment variable first, then user-supplied path, then fallback
    if(NOT DEFINED ZMQ_ROOT AND DEFINED ENV{ZMQ_ROOT})
        set(ZMQ_ROOT $ENV{ZMQ_ROOT})
    endif()
    
    if(NOT ZMQ_ROOT)
        set(ZMQ_ROOT "D:/SDK/ZeroMQ" CACHE PATH "ZeroMQ root directory (fallback)")
        message(STATUS "Using fallback ZMQ_ROOT: ${ZMQ_ROOT}")
    else()
        message(STATUS "Using ZMQ_ROOT: ${ZMQ_ROOT}")
    endif()

    set(ZMQ_INCLUDE_DIR "${ZMQ_ROOT}/include")
    set(ZMQ_LIBRARY "${ZMQ_ROOT}/lib/libzmq-v143-mt-4_3_6.lib")
    
    # Verify ZeroMQ installation
    if(NOT EXISTS "${ZMQ_INCLUDE_DIR}/zmq.h")
        message(FATAL_ERROR "ZeroMQ headers not found at ${ZMQ_INCLUDE_DIR}. Please set ZMQ_ROOT correctly.")
    endif()
    if(NOT EXISTS "${ZMQ_LIBRARY}")
        message(FATAL_ERROR "ZeroMQ library not found at ${ZMQ_LIBRARY}. Please set ZMQ_ROOT correctly.")
    endif()
else()
    # Linux: Use pkg-config first, then fallback to find_path/find_library
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(ZMQ libzmq)
        if(ZMQ_FOUND)
            set(ZMQ_INCLUDE_DIR ${ZMQ_INCLUDE_DIRS})
            set(ZMQ_LIBRARY ${ZMQ_LIBRARIES})
            message(STATUS "Found ZeroMQ via pkg-config: ${ZMQ_LIBRARY}")
        endif()
    endif()
    
    # Fallback if pkg-config didn't work
    if(NOT ZMQ_INCLUDE_DIR)
        find_path(ZMQ_INCLUDE_DIR zmq.h
            HINTS /usr/include /usr/local/include
            PATH_SUFFIXES zmq
        )
    endif()
    if(NOT ZMQ_LIBRARY)
        find_library(ZMQ_LIBRARY NAMES zmq libzmq
            HINTS /usr/lib /usr/local/lib
            PATH_SUFFIXES x86_64-linux-gnu
        )
    endif()
endif()

if(NOT ZMQ_INCLUDE_DIR OR NOT ZMQ_LIBRARY)
    message(FATAL_ERROR "ZeroMQ not found. Please install ZeroMQ or set ZMQ_ROOT environment variable.")
endif()

message(STATUS "ZeroMQ Include: ${ZMQ_INCLUDE_DIR}")
message(STATUS "ZeroMQ Library: ${ZMQ_LIBRARY}")

# ------------------------------
# OpenSSL Configuration (Portable)
# ------------------------------
# Try environment variable first, then user-supplied path, then fallback (Windows only)
if(NOT DEFINED OPENSSL_ROOT_DIR AND DEFINED ENV{OPENSSL_ROOT_DIR})
    set(OPENSSL_ROOT_DIR $ENV{OPENSSL_ROOT_DIR})
endif()

if(WIN32 AND NOT OPENSSL_ROOT_DIR)
    set(OPENSSL_ROOT_DIR "C:/Program Files/FireDaemon OpenSSL 3" CACHE PATH "OpenSSL root directory (fallback)")
    message(STATUS "Using fallback OPENSSL_ROOT_DIR: ${OPENSSL_ROOT_DIR}")
elseif(OPENSSL_ROOT_DIR)
    message(STATUS "Using OPENSSL_ROOT_DIR: ${OPENSSL_ROOT_DIR}")
endif()

find_package(OpenSSL REQUIRED)
message(STATUS "OpenSSL Include: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "OpenSSL Libraries: ${OPENSSL_LIBRARIES}")

# ------------------------------
# Platform-specific configurations
# ------------------------------
if(WIN32)
    # Define export macros for Windows
    add_definitions(-DANARI_USD_MIDDLEWARE_API=__declspec\(dllexport\))
    
    # Use dynamic runtime linking for MSVC
    if(MSVC)
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MD")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MDd")
    endif()
else()
    # Define export macros for Linux
    add_definitions(-DANARI_USD_MIDDLEWARE_API=__attribute__\(\(visibility\(\"default\"\)\)\))
endif()

# ------------------------------
# Build Configuration
# ------------------------------
# Build as a shared library
add_library(${PROJECT_NAME} SHARED
    src/AnariUsdMiddleware.cpp
    src/ZmqConnector.cpp
    src/HashVerifier.cpp
    src/UsdProcessor.cpp
)

# Add include directories
target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        ${CMAKE_CURRENT_SOURCE_DIR}/external/glm
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/tinyusdz/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/stb
        ${ZMQ_INCLUDE_DIR}
        ${OPENSSL_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/external/cppzmq
)

# Add external dependencies
add_subdirectory(external/tinyusdz external/tinyusdz_build)
add_subdirectory(external/glm external/glm_build)

# Link dependencies
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        ${ZMQ_LIBRARY}
        OpenSSL::SSL
        OpenSSL::Crypto
        tinyusdz::tinyusdz_static
        glm::glm
)

# Add platform-specific libraries
if(WIN32)
    target_link_libraries(${PROJECT_NAME}
        PRIVATE
            ws2_32
            iphlpapi
    )
endif()

# ------------------------------
# Optional Components
# ------------------------------
# JUSYNC GUI option
option(BUILD_JUSYNC_Receiver_GUI "Build the Dear ImGui GUI for the middleware" OFF)
if(BUILD_JUSYNC_Receiver_GUI)
    add_subdirectory(tools/ReceiverUI)
endif()

# Test executable option
option(BUILD_TESTS "Build test applications" ON)
if(BUILD_TESTS)
    add_executable(test_middleware src/test/test_middleware.cpp)
    target_link_libraries(test_middleware PRIVATE ${PROJECT_NAME})
    
    if(WIN32)
        # Copy required DLLs to test executable directory
        set(REQUIRED_DLLS
            "${ZMQ_ROOT}/bin/libzmq-v143-mt-4_3_6.dll"
            "${OPENSSL_ROOT_DIR}/bin/libcrypto-3-x64.dll"
            "${OPENSSL_ROOT_DIR}/bin/libssl-3-x64.dll"
        )
        
        foreach(DLL ${REQUIRED_DLLS})
            if(EXISTS "${DLL}")
                add_custom_command(TARGET test_middleware POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DLL}"
                    $<TARGET_FILE_DIR:test_middleware>
                )
            else()
                message(WARNING "Required DLL not found: ${DLL}")
            endif()
        endforeach()
    endif()
endif()

# ------------------------------
# Installation Rules
# ------------------------------
install(TARGETS ${PROJECT_NAME}
    EXPORT ${PROJECT_NAME}Targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(DIRECTORY include/ DESTINATION include)

# Install required DLLs on Windows
if(WIN32)
    set(INSTALL_DLLS
        "${ZMQ_ROOT}/bin/libzmq-v143-mt-4_3_6.dll"
        "${OPENSSL_ROOT_DIR}/bin/libcrypto-3-x64.dll"
        "${OPENSSL_ROOT_DIR}/bin/libssl-3-x64.dll"
    )
    
    foreach(DLL ${INSTALL_DLLS})
        if(EXISTS "${DLL}")
            install(FILES "${DLL}" DESTINATION bin)
        else()
            message(WARNING "Installation DLL not found: ${DLL}")
        endif()
    endforeach()
endif()

# Generate and install CMake config files
include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${PROJECT_NAME}Config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
    INSTALL_DESTINATION lib/cmake/${PROJECT_NAME}
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT ${PROJECT_NAME}Targets
    FILE ${PROJECT_NAME}Targets.cmake
    NAMESPACE ${PROJECT_NAME}::
    DESTINATION lib/cmake/${PROJECT_NAME}
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
    DESTINATION lib/cmake/${PROJECT_NAME}
)

# ------------------------------
# Summary
# ------------------------------
message(STATUS "")
message(STATUS "=== ${PROJECT_NAME} Configuration Summary ===")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "")
message(STATUS "Dependencies:")
message(STATUS "  ZeroMQ Include: ${ZMQ_INCLUDE_DIR}")
message(STATUS "  ZeroMQ Library: ${ZMQ_LIBRARY}")
message(STATUS "  OpenSSL Include: ${OPENSSL_INCLUDE_DIR}")
message(STATUS "  OpenSSL Libraries: ${OPENSSL_LIBRARIES}")
message(STATUS "")
message(STATUS "Options:")
message(STATUS "  BUILD_TESTS: ${BUILD_TESTS}")
message(STATUS "  BUILD_JUSYNC_Receiver_GUI: ${BUILD_JUSYNC_Receiver_GUI}")
message(STATUS "")
message(STATUS "To override paths, use:")
message(STATUS "  cmake -DZMQ_ROOT=/path/to/zmq -DOPENSSL_ROOT_DIR=/path/to/openssl ..")
message(STATUS "Or set environment variables ZMQ_ROOT and OPENSSL_ROOT_DIR")
message(STATUS "===========================================")
