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
    file(DOWNLOAD https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h ${MINIAUDIO_H}
         STATUS MINIAUDIO_DOWNLOAD_STATUS
         TLS_VERIFY ON)
    list(GET MINIAUDIO_DOWNLOAD_STATUS 0 MINIAUDIO_DOWNLOAD_CODE)
    if(NOT MINIAUDIO_DOWNLOAD_CODE EQUAL 0)
        file(REMOVE ${MINIAUDIO_H})
        message(FATAL_ERROR "Failed to download miniaudio.h (status: ${MINIAUDIO_DOWNLOAD_STATUS}). "
                             "Check your internet connection/firewall, or manually download it from "
                             "https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h "
                             "and place it at ${MINIAUDIO_H}")
    endif()
    file(SIZE ${MINIAUDIO_H} MINIAUDIO_FILE_SIZE)
    if(MINIAUDIO_FILE_SIZE LESS 1000000)
        message(FATAL_ERROR "Downloaded miniaudio.h looks incomplete/corrupt (only ${MINIAUDIO_FILE_SIZE} bytes). "
                             "Delete ${MINIAUDIO_H} and re-run CMake, or download it manually from "
                             "https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h")
    endif()
endif()

# 3b. Download stb_image.h directly (decodes the ad banner image at runtime)
set(STB_DIR ${CMAKE_BINARY_DIR}/stb)
file(MAKE_DIRECTORY ${STB_DIR})
set(STB_IMAGE_H ${STB_DIR}/stb_image.h)
if(NOT EXISTS ${STB_IMAGE_H})
    message(STATUS "Downloading stb_image.h...")
    file(DOWNLOAD https://raw.githubusercontent.com/nothings/stb/master/stb_image.h ${STB_IMAGE_H}
         STATUS STB_DOWNLOAD_STATUS
         TLS_VERIFY ON)
    list(GET STB_DOWNLOAD_STATUS 0 STB_DOWNLOAD_CODE)
    if(NOT STB_DOWNLOAD_CODE EQUAL 0)
        file(REMOVE ${STB_IMAGE_H})
        message(FATAL_ERROR "Failed to download stb_image.h (status: ${STB_DOWNLOAD_STATUS}). "
                             "Check your internet connection/firewall, or manually download it from "
                             "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h "
                             "and place it at ${STB_IMAGE_H}")
    endif()
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
    ${STB_DIR}
)

# Link Libraries
target_link_libraries(voice-changer PRIVATE
    glfw
    OpenGL::GL
)

# App version (sent to the license server for per-key analytics)
target_compile_definitions(voice-changer PRIVATE APP_VERSION="2.2.0")

# Windows-specific configurations
if(WIN32)
    # ole32/uuid/winmm: miniaudio; winhttp: license verification;
    # shell32: open browser; advapi32: read MachineGuid for device binding
    target_link_libraries(voice-changer PRIVATE
        ole32
        uuid
        winmm
        winhttp
        shell32
        advapi32
    )

    # CRITICAL: statically link the MinGW C/C++ runtime and threading library.
    # Without this the exe depends on libstdc++-6.dll, libgcc_s_seh-1.dll and
    # libwinpthread-1.dll, which exist only on a machine with MinGW installed -
    # so it runs on the dev box but crashes with "DLL not found" on every
    # user's PC. -static pulls all three into the exe, making it self-contained.
    target_link_options(voice-changer PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
    )

    # Embed the application icon
    enable_language(RC)
    target_sources(voice-changer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/installer/app.rc)
endif()
