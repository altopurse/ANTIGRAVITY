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

# 4. Fetch RNNoise (deep-learning noise suppression, BSD-licensed).
# The v0.2 release tarball ships the pretrained model as generated C sources
# (rnnoise_data.c), so no model download step is needed. Downloaded manually
# (not FetchContent) because CMake's bundled GnuTLS can't verify GitHub's cert
# chain on some MinGW installs - the system curl can, and the pinned SHA256 is
# enforced regardless of which downloader fetched the file.
set(RNNOISE_URL "https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz")
set(RNNOISE_SHA256 "90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37")
set(RNNOISE_TARBALL ${CMAKE_BINARY_DIR}/rnnoise-0.2.tar.gz)
set(rnnoise_SOURCE_DIR ${CMAKE_BINARY_DIR}/rnnoise-0.2)

if(NOT EXISTS ${rnnoise_SOURCE_DIR}/src/denoise.c)
    if(NOT EXISTS ${RNNOISE_TARBALL})
        message(STATUS "Downloading RNNoise 0.2...")
        file(DOWNLOAD ${RNNOISE_URL} ${RNNOISE_TARBALL} STATUS RNNOISE_DL_STATUS TLS_VERIFY ON)
        list(GET RNNOISE_DL_STATUS 0 RNNOISE_DL_CODE)
        if(NOT RNNOISE_DL_CODE EQUAL 0)
            file(REMOVE ${RNNOISE_TARBALL})
            find_program(CURL_EXE curl)
            if(CURL_EXE)
                message(STATUS "CMake download failed, retrying with system curl...")
                execute_process(COMMAND ${CURL_EXE} -fsSL -o ${RNNOISE_TARBALL} ${RNNOISE_URL}
                                RESULT_VARIABLE RNNOISE_CURL_CODE)
                if(NOT RNNOISE_CURL_CODE EQUAL 0)
                    file(REMOVE ${RNNOISE_TARBALL})
                endif()
            endif()
        endif()
        if(NOT EXISTS ${RNNOISE_TARBALL})
            message(FATAL_ERROR "Failed to download RNNoise. Download it manually from "
                                "${RNNOISE_URL} and place it at ${RNNOISE_TARBALL}, then re-run CMake.")
        endif()
    endif()
    # Integrity check before extracting anything, whatever fetched the file
    file(SHA256 ${RNNOISE_TARBALL} RNNOISE_ACTUAL_SHA256)
    if(NOT RNNOISE_ACTUAL_SHA256 STREQUAL RNNOISE_SHA256)
        file(REMOVE ${RNNOISE_TARBALL})
        message(FATAL_ERROR "RNNoise tarball hash mismatch (got ${RNNOISE_ACTUAL_SHA256}). "
                            "Deleted the bad file - re-run CMake to retry the download.")
    endif()
    file(ARCHIVE_EXTRACT INPUT ${RNNOISE_TARBALL} DESTINATION ${CMAKE_BINARY_DIR})
endif()

# Plain-C portable build: everything in src/ except the two training/dev tools
# (which have their own main()) and the x86/ runtime-dispatch variants.
set(RNNOISE_SOURCES
    ${rnnoise_SOURCE_DIR}/src/denoise.c
    ${rnnoise_SOURCE_DIR}/src/rnn.c
    ${rnnoise_SOURCE_DIR}/src/pitch.c
    ${rnnoise_SOURCE_DIR}/src/kiss_fft.c
    ${rnnoise_SOURCE_DIR}/src/celt_lpc.c
    ${rnnoise_SOURCE_DIR}/src/nnet.c
    ${rnnoise_SOURCE_DIR}/src/nnet_default.c
    ${rnnoise_SOURCE_DIR}/src/parse_lpcnet_weights.c
    ${rnnoise_SOURCE_DIR}/src/rnnoise_data.c
    ${rnnoise_SOURCE_DIR}/src/rnnoise_tables.c
)

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
    ${RNNOISE_SOURCES}
)

# Include Directories
target_include_directories(voice-changer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${MINIAUDIO_DIR}
    ${STB_DIR}
    ${rnnoise_SOURCE_DIR}/include
    ${rnnoise_SOURCE_DIR}/src
)

# Link Libraries
target_link_libraries(voice-changer PRIVATE
    glfw
    OpenGL::GL
)

# App version (sent to the license server for per-key analytics)
target_compile_definitions(voice-changer PRIVATE APP_VERSION="2.5.0")

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
