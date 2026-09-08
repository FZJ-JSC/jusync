# tests/CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

# USD Validation Tool - No ZMQ Dependencies
add_executable(usd_validation_tool
    src/usd_validation_tool.cpp
)

target_link_libraries(usd_validation_tool
    PRIVATE
        ${PROJECT_NAME}  # Main middleware library
)

target_include_directories(usd_validation_tool PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# Set C++17 standard
target_compile_features(usd_validation_tool PRIVATE cxx_std_17)

# Copy ONLY comprehensive_test.sh to build directory
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/comprehensive_test.sh
    ${CMAKE_BINARY_DIR}/comprehensive_test.sh
    COPYONLY
)

# Make script executable on Unix systems
if(UNIX)
    add_custom_command(TARGET usd_validation_tool POST_BUILD
        COMMAND chmod +x ${CMAKE_BINARY_DIR}/comprehensive_test.sh
    )
endif()

# Create test data directory structure in build
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_data/usd_samples)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test_reports)

# Copy USD test data files
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/usd_samples")
    file(GLOB USD_TEST_FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/usd_samples/*.usda")
    foreach(USD_FILE ${USD_TEST_FILES})
        configure_file(${USD_FILE} ${CMAKE_BINARY_DIR}/test_data/usd_samples/ COPYONLY)
    endforeach()
endif()

# Enable CTest integration
enable_testing()

# Add comprehensive USD validation tests
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/usd_samples")
    file(GLOB USD_TEST_FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/usd_samples/*.usda")

    foreach(USD_FILE ${USD_TEST_FILES})
        get_filename_component(TEST_NAME ${USD_FILE} NAME_WE)

        # Test basic USD loading
        add_test(NAME "usd_load_${TEST_NAME}"
                 COMMAND usd_validation_tool ${CMAKE_BINARY_DIR}/test_data/usd_samples/${TEST_NAME}.usda
                 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

        # Test with collision generation
        add_test(NAME "usd_collision_${TEST_NAME}"
                 COMMAND usd_validation_tool ${CMAKE_BINARY_DIR}/test_data/usd_samples/${TEST_NAME}.usda --collision=3
                 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    endforeach()

    # Set test timeout
    set_tests_properties(${USD_TEST_FILES} PROPERTIES TIMEOUT 120)
endif()

# Comprehensive test suite target
add_custom_target(run_comprehensive_tests
    COMMAND ${CMAKE_BINARY_DIR}/comprehensive_test.sh
    DEPENDS usd_validation_tool
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running comprehensive USD validation tests"
)
