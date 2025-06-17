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

# Find OpenGL (cross-platform)
find_package(OpenGL REQUIRED)

# ------------------------------
# GLFW Configuration (Dynamic)
# ------------------------------
if(WIN32)
    # Windows: Try find_package first, then fallback to manual paths
    find_package(glfw3 QUIET)
    if(NOT glfw3_FOUND)
        # Try environment variable or user-supplied path
        if(NOT DEFINED GLFW_ROOT AND DEFINED ENV{GLFW_ROOT})
            set(GLFW_ROOT $ENV{GLFW_ROOT})
        endif()
        
        if(NOT GLFW_ROOT)
            set(GLFW_ROOT "C:/SDK/glfw" CACHE PATH "GLFW root directory (fallback)")
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
            set(GLFW_FOUND TRUE)
            set(GLFW_LIBRARIES ${GLFW_LIBRARY})
            set(GLFW_INCLUDE_DIRS ${GLFW_INCLUDE_DIR})
        endif()
    else()
        set(GLFW_FOUND TRUE)
        set(GLFW_LIBRARIES glfw)
        get_target_property(GLFW_INCLUDE_DIRS glfw INTERFACE_INCLUDE_DIRECTORIES)
    endif()
else()
    # Linux: Use pkg-config first, then fallback
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(GLFW glfw3)
    endif()
    
    if(NOT GLFW_FOUND)
        find_package(glfw3 QUIET)
        if(glfw3_FOUND)
            set(GLFW_FOUND TRUE)
            set(GLFW_LIBRARIES glfw)
        endif()
    endif()
    
    # Final fallback
    if(NOT GLFW_FOUND)
        find_path(GLFW_INCLUDE_DIR GLFW/glfw3.h)
        find_library(GLFW_LIBRARY NAMES glfw3 glfw)
        if(GLFW_INCLUDE_DIR AND GLFW_LIBRARY)
            set(GLFW_FOUND TRUE)
            set(GLFW_LIBRARIES ${GLFW_LIBRARY})
            set(GLFW_INCLUDE_DIRS ${GLFW_INCLUDE_DIR})
        endif()
    endif()
endif()

if(NOT GLFW_FOUND)
    message(FATAL_ERROR "GLFW not found. Please install GLFW or set GLFW_ROOT.")
endif()

# ------------------------------
# GLEW Configuration (Dynamic)
# ------------------------------
if(WIN32)
    # Windows: Try find_package first
    find_package(GLEW QUIET)
    if(NOT GLEW_FOUND)
        if(NOT DEFINED GLEW_ROOT AND DEFINED ENV{GLEW_ROOT})
            set(GLEW_ROOT $ENV{GLEW_ROOT})
        endif()
        
        if(NOT GLEW_ROOT)
            set(GLEW_ROOT "C:/SDK/glew" CACHE PATH "GLEW root directory (fallback)")
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
            set(GLEW_FOUND TRUE)
            set(GLEW_LIBRARIES ${GLEW_LIBRARY})
            set(GLEW_INCLUDE_DIRS ${GLEW_INCLUDE_DIR})
        endif()
    else()
        set(GLEW_LIBRARIES GLEW::GLEW)
        get_target_property(GLEW_INCLUDE_DIRS GLEW::GLEW INTERFACE_INCLUDE_DIRECTORIES)
    endif()
else()
    # Linux: Use pkg-config first
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(GLEW glew)
    endif()
    
    if(NOT GLEW_FOUND)
        find_package(GLEW QUIET)
        if(GLEW_FOUND)
            set(GLEW_LIBRARIES GLEW::GLEW)
        endif()
    endif()
    
    # Final fallback
    if(NOT GLEW_FOUND)
        find_path(GLEW_INCLUDE_DIR GL/glew.h)
        find_library(GLEW_LIBRARY NAMES GLEW glew)
        if(GLEW_INCLUDE_DIR AND GLEW_LIBRARY)
            set(GLEW_FOUND TRUE)
            set(GLEW_LIBRARIES ${GLEW_LIBRARY})
            set(GLEW_INCLUDE_DIRS ${GLEW_INCLUDE_DIR})
        endif()
    endif()
endif()

if(NOT GLEW_FOUND)
    message(FATAL_ERROR "GLEW not found. Please install GLEW or set GLEW_ROOT.")
endif()

# ------------------------------
# ImGui Configuration
# ------------------------------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# ------------------------------
# Build Configuration
# ------------------------------
add_executable(ReceiverUI main.cpp ${IMGUI_SOURCES})

target_include_directories(ReceiverUI PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${GLFW_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(ReceiverUI PRIVATE
    anari_usd_middleware
    ${OPENGL_LIBRARIES}
    ${GLFW_LIBRARIES}
    ${GLEW_LIBRARIES}
)

# Platform-specific libraries
if(WIN32)
    # Windows-specific libraries if needed
    target_link_libraries(ReceiverUI PRIVATE)
else()
    # Linux-specific libraries
    target_link_libraries(ReceiverUI PRIVATE
        pthread
        dl
    )
endif()

# Add compile flags
if(GLFW_CFLAGS_OTHER)
    target_compile_options(ReceiverUI PRIVATE ${GLFW_CFLAGS_OTHER})
endif()
if(GLEW_CFLAGS_OTHER)
    target_compile_options(ReceiverUI PRIVATE ${GLEW_CFLAGS_OTHER})
endif()

# ------------------------------
# Summary
# ------------------------------
message(STATUS "")
message(STATUS "=== ReceiverUI Configuration ===")
message(STATUS "GLFW Found: ${GLFW_FOUND}")
message(STATUS "GLFW Include: ${GLFW_INCLUDE_DIRS}")
message(STATUS "GLFW Libraries: ${GLFW_LIBRARIES}")
message(STATUS "GLEW Found: ${GLEW_FOUND}")
message(STATUS "GLEW Include: ${GLEW_INCLUDE_DIRS}")
message(STATUS "GLEW Libraries: ${GLEW_LIBRARIES}")
message(STATUS "===============================")
