cmake_minimum_required(VERSION 3.16)
project(anari_usd_middleware VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GLIBC 2.35 compatibility settings for Linux
if(UNIX AND NOT APPLE)
    add_compile_definitions(
        _GLIBCXX_USE_CXX11_ABI=1
        _GNU_SOURCE
        _FORTIFY_SOURCE=2
    )

    # Enhanced build flags for compatibility
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -pipe -fPIC")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--as-needed -Wl,--no-undefined")
endif()

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
    src/AnariUsdMiddleware_C.cpp
    src/CollisionProcessor.cpp
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

    target_compile_definitions(${PROJECT_NAME} PRIVATE
        ANARI_USD_MIDDLEWARE_EXPORTS
    )
endif()

# Linux-specific compatibility enhancements
if(UNIX AND NOT APPLE)
    # Static link C++ runtime to avoid version conflicts
    target_link_options(${PROJECT_NAME} PRIVATE
        -static-libstdc++
        -static-libgcc
    )

    # Set RPATH so Unreal Engine can find the library
    set_target_properties(${PROJECT_NAME} PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_RPATH "$ORIGIN"
    )

    # Add standard Linux system libraries
    target_link_libraries(${PROJECT_NAME} PRIVATE
        pthread
        dl
        rt
        m
    )
endif()

# Custom target for compatibility verification (Linux only)
if(UNIX AND NOT APPLE)
    add_custom_target(verify_glibc_compatibility ALL
        DEPENDS ${PROJECT_NAME}
        COMMAND ${CMAKE_COMMAND} -E echo "=== Verifying GLIBC Compatibility ==="
        COMMAND objdump -T $<TARGET_FILE:${PROJECT_NAME}> | grep GLIBC_ | sort -u | tail -n1 || echo "No GLIBC symbols found"
        COMMAND ${CMAKE_COMMAND} -E echo "=== Checking Dependencies ==="
        COMMAND ldd $<TARGET_FILE:${PROJECT_NAME}> | grep "not found" || echo "All dependencies satisfied"
        COMMAND ${CMAKE_COMMAND} -E echo "=== Verifying Static Linking ==="
        COMMAND readelf -d $<TARGET_FILE:${PROJECT_NAME}> | grep NEEDED | grep stdc++ || echo "libstdc++ statically linked ✓"
        COMMAND ${CMAKE_COMMAND} -E echo "=== Compatibility Check Complete ==="
        VERBATIM
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

# USD Validation Test Tool
option(BUILD_TESTS "Build test and validation applications" ON)
if(BUILD_TESTS)
    add_executable(usd_validation_tool
        tests/src/usd_validation_tool.cpp
    )

    target_link_libraries(usd_validation_tool
        PRIVATE
            ${PROJECT_NAME}  # Your main middleware library
    )

    target_include_directories(usd_validation_tool PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    # Set C++17 standard for the test tool
    target_compile_features(usd_validation_tool PRIVATE cxx_std_17)

    # REMOVE: Copy test scripts to build directory
    # configure_file(
    #    ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/comprehensive_test.sh
    #    ${CMAKE_BINARY_DIR}/comprehensive_test.sh
    #    COPYONLY
    # )

    # REMOVE: Make scripts executable on Unix systems
    # if(UNIX)
    #    add_custom_command(TARGET usd_validation_tool POST_BUILD
    #        COMMAND chmod +x ${CMAKE_BINARY_DIR}/comprehensive_test.sh
    #    )
    # endif()

    # Create test data directory structure in build
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_data/usd_samples)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_reports)

    # Copy USD test data files if they exist
    # The usd_validation_tool.cpp handles reading the test data directly from the source directory.
    # We might not need to copy them to the binary directory if the tool is invoked with the source path.
    # However, if the tool expects them in the build directory, keep this part.
    # Based on usd_validation_tool.cpp, it expects the path to the original `tests/data/usd_samples`.
    # So, this `configure_file` for USD_TEST_FILES can likely be removed as well,
    # or modified to simply ensure the directory structure exists if the tool
    # expects to write reports there.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples")
        # No need to copy individual files if the C++ tool is given the source path.
        # This only creates the destination directories if they don't exist.
        # file(GLOB USD_TEST_FILES "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples/*.usda")
        # foreach(USD_FILE ${USD_TEST_FILES})
        #    configure_file(${USD_FILE} ${CMAKE_BINARY_DIR}/test_data/usd_samples/ COPYONLY)
        # endforeach()
    endif()

    # Enable CTest integration
    enable_testing()

    # Add individual tests for each USD file - these will run usd_validation_tool directly
    # This CTest setup makes sense, as it leverages the usd_validation_tool.
    # The usd_validation_tool itself can process a directory, so CTest could also just
    # run `usd_validation_tool ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples`
    # as a single test, rather than per-file.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples")
        file(GLOB USD_TEST_FILES "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples/*.usda")

        set(ALL_TEST_NAMES)

        foreach(USD_FILE ${USD_TEST_FILES})
            get_filename_component(TEST_NAME ${USD_FILE} NAME_WE)

            # Test basic USD loading
            add_test(NAME "usd_load_${TEST_NAME}"
                     COMMAND usd_validation_tool ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples/${TEST_NAME}.usda # Use source path
                     WORKING_DIRECTORY ${CMAKE_BINARY_DIR}) # Tool should output reports here

            list(APPEND ALL_TEST_NAMES "usd_load_${TEST_NAME}")
        endforeach()

        if(ALL_TEST_NAMES)
            set_tests_properties(${ALL_TEST_NAMES} PROPERTIES TIMEOUT 120)
        endif()
    endif()



    add_custom_target(run_validation_tool
        COMMAND usd_validation_tool ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples
        DEPENDS usd_validation_tool
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running comprehensive USD validation tests using usd_validation_tool"
    )


    # Windows DLL copying for validation tool (keep this as it's for the executable)
    if(WIN32)
        set(REQUIRED_DLLS
            "${ZMQ_ROOT}/bin/libzmq-v143-mt-4_3_6.dll"
            "${OPENSSL_ROOT_DIR}/bin/libcrypto-3-x64.dll"
            "${OPENSSL_ROOT_DIR}/bin/libssl-3-x64.dll"
        )

        foreach(DLL ${REQUIRED_DLLS})
            if(EXISTS "${DLL}")
                add_custom_command(TARGET usd_validation_tool POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DLL}"
                    $<TARGET_FILE_DIR:usd_validation_tool>
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

# Install USD validation tool if built
if(BUILD_TESTS)
    install(TARGETS usd_validation_tool
        RUNTIME DESTINATION bin
    )
endif()

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
message(STATUS "  C-wrapper: ENABLED")
if(BUILD_TESTS)
message(STATUS "")
message(STATUS "Test Tools:")
message(STATUS "  usd_validation_tool: Comprehensive USD validation")
message(STATUS "  comprehensive_test.sh: Full test suite")
message(STATUS "  run_usd_tests.sh: Quick USD tests")
message(STATUS "  CTest Integration: ENABLED")
endif()
if(UNIX AND NOT APPLE)
message(STATUS "")
message(STATUS "Linux Compatibility:")
message(STATUS "  GLIBC 2.35 compatibility: ENABLED")
message(STATUS "  Static C++ runtime linking: ENABLED")
message(STATUS "  RPATH configuration: $ORIGIN")
endif()
message(STATUS "")
message(STATUS "To override paths, use:")
message(STATUS "  cmake -DZMQ_ROOT=/path/to/zmq -DOPENSSL_ROOT_DIR=/path/to/openssl ..")
message(STATUS "Or set environment variables ZMQ_ROOT and OPENSSL_ROOT_DIR")
message(STATUS "===========================================")
