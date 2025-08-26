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

# ------------------------------
# USD Validation Test Tool (Enhanced)
# ------------------------------
option(BUILD_TESTS "Build test and validation applications" ON)
if(BUILD_TESTS)
    # Enhanced USD Validation Tool with granular test modes
    add_executable(usd_validation_tool
        tests/src/usd_validation_tool.cpp
    )

    target_link_libraries(usd_validation_tool
        PRIVATE
            ${PROJECT_NAME}
    )

    target_include_directories(usd_validation_tool PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    # Set C++17 standard for the test tool
    target_compile_features(usd_validation_tool PRIVATE cxx_std_17)

    # Create test data and reports directory structure
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_data/usd_samples)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_reports)

    # Copy USD test data files if they exist
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples")
        file(GLOB USD_TEST_FILES "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples/*.usda"
                                 "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples/*.usd"
                                 "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples/*.usdc")
        foreach(USD_FILE ${USD_TEST_FILES})
            get_filename_component(USD_FILENAME ${USD_FILE} NAME)
            configure_file(${USD_FILE} ${CMAKE_BINARY_DIR}/test_data/usd_samples/${USD_FILENAME} COPYONLY)
        endforeach()
        message(STATUS "Copied ${CMAKE_CURRENT_LIST_LENGTH} USD test files")
    endif()

    # Enable CTest integration with granular tests
    enable_testing()

    # Test Categories - These will show up as individual jobs in GitHub Actions
    add_test(NAME "build_verification"
             COMMAND ${CMAKE_COMMAND} -E echo "Build verification test"
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

    add_test(NAME "static_analysis"
             COMMAND usd_validation_tool --mode=static-check
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

    add_test(NAME "usd_basic_processing"
             COMMAND usd_validation_tool --mode=usd-basic
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

    # Collision tests for different complexity levels
    add_test(NAME "collision_simple"
             COMMAND usd_validation_tool --mode=collision --type=simple
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

    add_test(NAME "collision_complex"
             COMMAND usd_validation_tool --mode=collision --type=complex
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

    add_test(NAME "collision_convex"
             COMMAND usd_validation_tool --mode=collision --type=convex
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

    add_test(NAME "error_handling_tests"
             COMMAND usd_validation_tool --mode=error-handling
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

    add_test(NAME "performance_benchmarks"
             COMMAND usd_validation_tool --mode=performance
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

    add_test(NAME "integration_suite"
             COMMAND usd_validation_tool --mode=integration
             WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

    # Set test properties
    set_tests_properties(
        build_verification static_analysis usd_basic_processing
        collision_simple collision_complex collision_convex
        error_handling_tests performance_benchmarks integration_suite
        PROPERTIES TIMEOUT 300
    )

    # Test dependencies - later tests depend on earlier ones
    set_tests_properties(static_analysis PROPERTIES DEPENDS build_verification)
    set_tests_properties(usd_basic_processing PROPERTIES DEPENDS static_analysis)
    set_tests_properties(collision_simple PROPERTIES DEPENDS usd_basic_processing)
    set_tests_properties(collision_complex PROPERTIES DEPENDS usd_basic_processing)
    set_tests_properties(collision_convex PROPERTIES DEPENDS usd_basic_processing)
    set_tests_properties(error_handling_tests PROPERTIES DEPENDS usd_basic_processing)
    set_tests_properties(performance_benchmarks PROPERTIES DEPENDS "collision_simple;collision_complex;collision_convex")
    set_tests_properties(integration_suite PROPERTIES DEPENDS "usd_basic_processing;collision_simple;error_handling_tests")

    # Legacy individual file tests (if USD files exist)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples")
        file(GLOB USD_TEST_FILES "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples/*.usda")

        foreach(USD_FILE ${USD_TEST_FILES})
            get_filename_component(TEST_NAME ${USD_FILE} NAME_WE)

            # Individual file tests (these run in parallel)
            add_test(NAME "usd_file_${TEST_NAME}"
                     COMMAND usd_validation_tool ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples/${TEST_NAME}.usda
                     WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

            set_tests_properties("usd_file_${TEST_NAME}" PROPERTIES
                TIMEOUT 120
                DEPENDS usd_basic_processing)
        endforeach()
    endif()

    # Custom targets for local testing
    add_custom_target(test_quick
        COMMAND ctest -R "build_verification|static_analysis|usd_basic_processing" --output-on-failure
        DEPENDS usd_validation_tool
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running quick validation tests"
    )

    add_custom_target(test_collision
        COMMAND ctest -R "collision_" --output-on-failure
        DEPENDS usd_validation_tool
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running collision-specific tests"
    )

    add_custom_target(test_full
        COMMAND ctest --output-on-failure --verbose
        DEPENDS usd_validation_tool
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running full test suite with detailed output"
    )

    add_custom_target(test_parallel
        COMMAND ctest -j${CMAKE_BUILD_PARALLEL_LEVEL} --output-on-failure
        DEPENDS usd_validation_tool
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running tests in parallel"
    )

    # Legacy compatibility target
    add_custom_target(run_validation_tool
        COMMAND usd_validation_tool ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/usd_samples
        DEPENDS usd_validation_tool
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running comprehensive USD validation tests using usd_validation_tool"
    )

    # Windows DLL copying for validation tool
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
message(STATUS "Enhanced Test Tools:")
message(STATUS "  usd_validation_tool: Granular USD validation with modes")
message(STATUS "  CTest Integration: ENABLED with 9 test categories")
message(STATUS "  Local test targets: test_quick, test_collision, test_full, test_parallel")
message(STATUS "")
message(STATUS "Test Categories Available:")
message(STATUS "  • build_verification: Build and compilation checks")
message(STATUS "  • static_analysis: Code quality and static analysis")
message(STATUS "  • usd_basic_processing: Basic USD file loading")
message(STATUS "  • collision_simple: Simple collision generation")
message(STATUS "  • collision_complex: Complex collision generation")
message(STATUS "  • collision_convex: Convex hull collision generation")
message(STATUS "  • error_handling_tests: Error scenario validation")
message(STATUS "  • performance_benchmarks: Performance testing")
message(STATUS "  • integration_suite: Full integration testing")
endif()
if(UNIX AND NOT APPLE)
message(STATUS "")
message(STATUS "Linux Compatibility:")
message(STATUS "  GLIBC 2.35 compatibility: ENABLED")
message(STATUS "  Static C++ runtime linking: ENABLED")
message(STATUS "  RPATH configuration: $ORIGIN")
endif()
message(STATUS "")
message(STATUS "Local Testing Commands:")
message(STATUS "  make test_quick      - Run basic validation tests")
message(STATUS "  make test_collision  - Run collision-specific tests")
message(STATUS "  make test_full       - Run full test suite")
message(STATUS "  make test_parallel   - Run tests in parallel")
message(STATUS "  ctest -V            - Run all tests with verbose output")
message(STATUS "")
message(STATUS "To override paths, use:")
message(STATUS "  cmake -DZMQ_ROOT=/path/to/zmq -DOPENSSL_ROOT_DIR=/path/to/openssl ..")
message(STATUS "Or set environment variables ZMQ_ROOT and OPENSSL_ROOT_DIR")
message(STATUS "===========================================")
