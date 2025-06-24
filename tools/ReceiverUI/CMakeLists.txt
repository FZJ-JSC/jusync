cmake_minimum_required(VERSION 3.16)
project(ReceiverUI)

# Set OpenGL preference and policy to avoid warnings
if(POLICY CMP0072)
    cmake_policy(SET CMP0072 NEW)
endif()
set(OpenGL_GL_PREFERENCE GLVND)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------------------
# Build Context Detection
# ------------------------------
# Check if we're building as part of the main project or standalone
get_directory_property(HAS_PARENT PARENT_DIRECTORY)

if(HAS_PARENT)
    message(STATUS "Building ReceiverUI as part of main project")
    set(BUILDING_STANDALONE FALSE)
else()
    message(STATUS "Building ReceiverUI standalone")
    set(BUILDING_STANDALONE TRUE)
endif()

# ------------------------------
# Dependencies Configuration
# ------------------------------
find_package(OpenGL REQUIRED)

# GLFW Configuration (Modern CMake approach)
if(WIN32)
    find_package(glfw3 QUIET)
    if(NOT glfw3_FOUND)
        if(NOT DEFINED GLFW_ROOT AND DEFINED ENV{GLFW_ROOT})
            set(GLFW_ROOT $ENV{GLFW_ROOT})
        endif()

        if(NOT GLFW_ROOT)
            set(GLFW_ROOT "C:/SDK/glfw" CACHE PATH "GLFW root directory")
        endif()

        find_path(GLFW_INCLUDE_DIR GLFW/glfw3.h
                HINTS ${GLFW_ROOT}/include
                PATHS "C:/SDK/glfw/include" "D:/SDK/glfw/include"
        )

        find_library(GLFW_LIBRARY NAMES glfw3 glfw
                HINTS ${GLFW_ROOT}/lib
                PATHS "C:/SDK/glfw/lib" "D:/SDK/glfw/lib"
        )

        if(GLFW_INCLUDE_DIR AND GLFW_LIBRARY)
            add_library(glfw_imported INTERFACE)
            target_include_directories(glfw_imported INTERFACE ${GLFW_INCLUDE_DIR})
            target_link_libraries(glfw_imported INTERFACE ${GLFW_LIBRARY})
            set(GLFW_TARGET glfw_imported)
        else()
            message(FATAL_ERROR "GLFW not found. Please install GLFW or set GLFW_ROOT.")
        endif()
    else()
        set(GLFW_TARGET glfw)
    endif()
else()
    # Linux: Use pkg-config first, then fallback
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(GLFW glfw3)
        if(GLFW_FOUND)
            add_library(glfw_imported INTERFACE)
            target_include_directories(glfw_imported INTERFACE ${GLFW_INCLUDE_DIRS})
            target_link_libraries(glfw_imported INTERFACE ${GLFW_LIBRARIES})
            target_compile_options(glfw_imported INTERFACE ${GLFW_CFLAGS_OTHER})
            set(GLFW_TARGET glfw_imported)
        endif()
    endif()

    if(NOT GLFW_TARGET)
        find_package(glfw3 QUIET)
        if(glfw3_FOUND)
            set(GLFW_TARGET glfw)
        else()
            find_path(GLFW_INCLUDE_DIR GLFW/glfw3.h)
            find_library(GLFW_LIBRARY NAMES glfw3 glfw)
            if(GLFW_INCLUDE_DIR AND GLFW_LIBRARY)
                add_library(glfw_imported INTERFACE)
                target_include_directories(glfw_imported INTERFACE ${GLFW_INCLUDE_DIR})
                target_link_libraries(glfw_imported INTERFACE ${GLFW_LIBRARY})
                set(GLFW_TARGET glfw_imported)
            else()
                message(FATAL_ERROR "GLFW not found. Please install GLFW.")
            endif()
        endif()
    endif()
endif()

# GLEW Configuration (Modern CMake approach)
if(WIN32)
    find_package(GLEW QUIET)
    if(NOT GLEW_FOUND)
        if(NOT DEFINED GLEW_ROOT AND DEFINED ENV{GLEW_ROOT})
            set(GLEW_ROOT $ENV{GLEW_ROOT})
        endif()

        if(NOT GLEW_ROOT)
            set(GLEW_ROOT "C:/SDK/glew" CACHE PATH "GLEW root directory")
        endif()

        find_path(GLEW_INCLUDE_DIR GL/glew.h
                HINTS ${GLEW_ROOT}/include
                PATHS "C:/SDK/glew/include" "D:/SDK/glew/include"
        )

        find_library(GLEW_LIBRARY NAMES glew32 GLEW
                HINTS ${GLEW_ROOT}/lib
                PATHS "C:/SDK/glew/lib" "D:/SDK/glew/lib"
        )

        if(GLEW_INCLUDE_DIR AND GLEW_LIBRARY)
            add_library(glew_imported INTERFACE)
            target_include_directories(glew_imported INTERFACE ${GLEW_INCLUDE_DIR})
            target_link_libraries(glew_imported INTERFACE ${GLEW_LIBRARY})
            set(GLEW_TARGET glew_imported)
        else()
            message(FATAL_ERROR "GLEW not found. Please install GLEW or set GLEW_ROOT.")
        endif()
    else()
        set(GLEW_TARGET GLEW::GLEW)
    endif()
else()
    # Linux: Use pkg-config first
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(GLEW glew)
        if(GLEW_FOUND)
            add_library(glew_imported INTERFACE)
            target_include_directories(glew_imported INTERFACE ${GLEW_INCLUDE_DIRS})
            target_link_libraries(glew_imported INTERFACE ${GLEW_LIBRARIES})
            target_compile_options(glew_imported INTERFACE ${GLEW_CFLAGS_OTHER})
            set(GLEW_TARGET glew_imported)
        endif()
    endif()

    if(NOT GLEW_TARGET)
        find_package(GLEW QUIET)
        if(GLEW_FOUND)
            set(GLEW_TARGET GLEW::GLEW)
        else()
            find_path(GLEW_INCLUDE_DIR GL/glew.h)
            find_library(GLEW_LIBRARY NAMES GLEW glew)
            if(GLEW_INCLUDE_DIR AND GLEW_LIBRARY)
                add_library(glew_imported INTERFACE)
                target_include_directories(glew_imported INTERFACE ${GLEW_INCLUDE_DIR})
                target_link_libraries(glew_imported INTERFACE ${GLEW_LIBRARY})
                set(GLEW_TARGET glew_imported)
            else()
                message(FATAL_ERROR "GLEW not found. Please install GLEW.")
            endif()
        endif()
    endif()
endif()

# ------------------------------
# ImGui Configuration (Modern approach with FetchContent)
# ------------------------------
include(FetchContent)

# Check if ImGui is available locally first
set(LOCAL_IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)
if(EXISTS ${LOCAL_IMGUI_DIR}/imgui.cpp)
    message(STATUS "Using local ImGui from: ${LOCAL_IMGUI_DIR}")
    set(IMGUI_DIR ${LOCAL_IMGUI_DIR})
else()
    message(STATUS "Local ImGui not found, downloading...")
    FetchContent_Declare(
            imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG v1.90.1
    )
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR ${imgui_SOURCE_DIR})
endif()

# Create ImGui interface library
add_library(imgui_interface INTERFACE)
target_sources(imgui_interface INTERFACE
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_interface INTERFACE
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
)

# ------------------------------
# Middleware Configuration (Smart detection)
# ------------------------------
if(BUILDING_STANDALONE)
    # Standalone build - look for built library
    set(MIDDLEWARE_LIB_DIR ${CMAKE_SOURCE_DIR}/../../build)
    set(MIDDLEWARE_INCLUDE_DIR ${CMAKE_SOURCE_DIR}/../../include)

    if(WIN32)
        set(MIDDLEWARE_LIB_PATH ${MIDDLEWARE_LIB_DIR}/anari_usd_middleware.lib)
        set(MIDDLEWARE_DLL_PATH ${MIDDLEWARE_LIB_DIR}/anari_usd_middleware.dll)
    else()
        set(MIDDLEWARE_LIB_PATH ${MIDDLEWARE_LIB_DIR}/libanari_usd_middleware.so)
        if(NOT EXISTS ${MIDDLEWARE_LIB_PATH})
            set(MIDDLEWARE_LIB_PATH ${MIDDLEWARE_LIB_DIR}/libanari_usd_middleware.a)
        endif()
    endif()

    if(NOT EXISTS ${MIDDLEWARE_LIB_PATH})
        message(FATAL_ERROR "Middleware library not found at: ${MIDDLEWARE_LIB_PATH}
Please build the middleware first:
  cd ${CMAKE_SOURCE_DIR}/../..
  mkdir -p build && cd build
  cmake ..
  make -j$(nproc)")
    endif()

    if(NOT EXISTS ${MIDDLEWARE_INCLUDE_DIR}/AnariUsdMiddleware_C.h)
        message(FATAL_ERROR "Middleware header not found: ${MIDDLEWARE_INCLUDE_DIR}/AnariUsdMiddleware_C.h")
    endif()

    # Create imported library for standalone build
    add_library(middleware_imported INTERFACE)
    target_include_directories(middleware_imported INTERFACE ${MIDDLEWARE_INCLUDE_DIR})
    target_link_libraries(middleware_imported INTERFACE ${MIDDLEWARE_LIB_PATH})
    set(MIDDLEWARE_TARGET middleware_imported)

    message(STATUS "Using standalone middleware library: ${MIDDLEWARE_LIB_PATH}")
else()
    # Building as part of main project - use target directly
    set(MIDDLEWARE_TARGET anari_usd_middleware)
    message(STATUS "Using middleware target: ${MIDDLEWARE_TARGET}")
endif()

# ------------------------------
# Build Configuration
# ------------------------------
add_executable(ReceiverUI main.cpp)

# Link all dependencies using modern CMake targets
target_link_libraries(ReceiverUI PRIVATE
        ${MIDDLEWARE_TARGET}
        imgui_interface
        ${GLFW_TARGET}
        ${GLEW_TARGET}
        OpenGL::GL
)

# Platform-specific libraries
if(WIN32)
    # Copy DLL if it exists and we're in standalone mode
    if(BUILDING_STANDALONE AND EXISTS ${MIDDLEWARE_DLL_PATH})
        add_custom_command(TARGET ReceiverUI POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MIDDLEWARE_DLL_PATH}"
                $<TARGET_FILE_DIR:ReceiverUI>
                COMMENT "Copying middleware DLL to output directory"
        )
    endif()
else()
    # Linux-specific libraries
    target_link_libraries(ReceiverUI PRIVATE
            pthread
            dl
    )
endif()

# Compiler-specific settings
if(MSVC)
    target_compile_options(ReceiverUI PRIVATE /W4)
    target_compile_definitions(ReceiverUI PRIVATE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
    )
else()
    target_compile_options(ReceiverUI PRIVATE
            -Wall -Wextra -Wpedantic
            -Wno-unused-parameter
            -Wno-missing-field-initializers
    )
endif()

# ------------------------------
# Summary
# ------------------------------
message(STATUS "")
message(STATUS "=== ReceiverUI Configuration Summary ===")
message(STATUS "Build Mode: ${CMAKE_BUILD_TYPE}")
message(STATUS "Building Standalone: ${BUILDING_STANDALONE}")
message(STATUS "GLFW Target: ${GLFW_TARGET}")
message(STATUS "GLEW Target: ${GLEW_TARGET}")
message(STATUS "ImGui Directory: ${IMGUI_DIR}")
message(STATUS "Middleware Target: ${MIDDLEWARE_TARGET}")
if(BUILDING_STANDALONE)
    message(STATUS "Middleware Library: ${MIDDLEWARE_LIB_PATH}")
    message(STATUS "Middleware Include: ${MIDDLEWARE_INCLUDE_DIR}")
endif()
message(STATUS "=======================================")
