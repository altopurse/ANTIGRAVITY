cmake_minimum_required(VERSION 3.20)
project(VoiceChanger LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Fetch Dependencies
include(FetchContent)

# 1. Fetch GLFW
message(STATUS "Fetching GLFW...")
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# 2. Fetch Dear ImGui
message(STATUS "Fetching Dear ImGui...")
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.8
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

# 3. Download miniaudio.h directly
set(MINIAUDIO_DIR ${CMAKE_BINARY_DIR}/miniaudio)
file(MAKE_DIRECTORY ${MINIAUDIO_DIR})
set(MINIAUDIO_H ${MINIAUDIO_DIR}/miniaudio.h)
if(NOT EXISTS ${MINIAUDIO_H})
    message(STATUS "Downloading miniaudio.h...")
    file(DOWNLOAD https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h ${MINIAUDIO_H})
endif()

# Find OpenGL
find_package(OpenGL REQUIRED)

# ImGui Source Files
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

# App Source Files
file(GLOB_RECURSE APP_SOURCES 
    "src/*.cpp"
    "src/*.h"
)

# Create Executable
add_executable(voice-changer 
    ${APP_SOURCES}
    ${IMGUI_SOURCES}
)

# Include Directories
target_include_directories(voice-changer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${MINIAUDIO_DIR}
)

# Link Libraries
target_link_libraries(voice-changer PRIVATE
    glfw
    OpenGL::GL
)

# Windows-specific configurations
if(WIN32)
    # Miniaudio requirements on Windows
    target_link_libraries(voice-changer PRIVATE
        ole32
        uuid
        winmm
    )
endif()
