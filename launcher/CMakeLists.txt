# Operation Troy launcher
#
# A small controller-driven front end: finds the player's own copy of the game,
# checks the build tools, extracts the game files, builds the recomp on the
# player's PC, and starts it. It contains no game code and no game assets, so
# it can be downloaded on its own from the repository's Releases page.
#
# Build (Windows):  cmake -S launcher -B out/launcher && cmake --build out/launcher --config Release
# It needs only the SDL3 and Dear ImGui sources from thirdparty/rexglue-sdk/thirdparty.

cmake_minimum_required(VERSION 3.25)
project(optroy_launcher VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# One self-contained .exe: no Visual C++ runtime install needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(OPTROY_LAUNCHER_HEADLESS
       "Offscreen-only build used to render screenshots on machines without a display" OFF)
option(OPTROY_LAUNCHER_TOOL "Also build optroy_launcher_tool (command-line checks of the extractor etc.)" OFF)

set(OPTROY_THIRDPARTY "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/rexglue-sdk/thirdparty")
if(NOT EXISTS "${OPTROY_THIRDPARTY}/sdl3/CMakeLists.txt" OR NOT EXISTS "${OPTROY_THIRDPARTY}/imgui/imgui.h")
    message(FATAL_ERROR
        "SDL3 and Dear ImGui sources are missing. From the repository root run:\n"
        "  git submodule update --init --depth 1 thirdparty/rexglue-sdk/thirdparty/sdl3 thirdparty/rexglue-sdk/thirdparty/imgui")
endif()

# --- SDL3: static, only what the launcher uses -----------------------------------
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_POWER OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_TRAY OFF CACHE BOOL "" FORCE)
if(OPTROY_LAUNCHER_HEADLESS)
    foreach(opt SDL_X11 SDL_WAYLAND SDL_KMSDRM SDL_OPENGL SDL_OPENGLES SDL_VULKAN SDL_RENDER_VULKAN
                SDL_HIDAPI SDL_LIBUDEV SDL_DBUS SDL_IBUS SDL_DIALOG)
        set(${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
    set(SDL_OFFSCREEN ON CACHE BOOL "" FORCE)
    set(SDL_DUMMYVIDEO ON CACHE BOOL "" FORCE)
endif()
add_subdirectory("${OPTROY_THIRDPARTY}/sdl3" sdl3 EXCLUDE_FROM_ALL)

# --- Dear ImGui (drawing and fonts; the launcher does its own menu navigation) ----
set(IMGUI_DIR "${OPTROY_THIRDPARTY}/imgui")
add_library(launcher_imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp")
target_include_directories(launcher_imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_compile_definitions(launcher_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS)
target_link_libraries(launcher_imgui PUBLIC SDL3::SDL3-static)

# --- Embedded fonts (SIL Open Font License, see fonts/OFL-*.txt) -------------------
include(cmake/EmbedFiles.cmake)
optroy_embed_files("${CMAKE_CURRENT_BINARY_DIR}/embedded_files.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/Anton-Regular.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/FiraSans-Regular.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/FiraSans-Medium.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/FiraSans-SemiBold.ttf"
    "${CMAKE_CURRENT_SOURCE_DIR}/fonts/AnonymousPro-Regular.ttf")

# --- The launcher -----------------------------------------------------------------
add_executable(optroy_launcher WIN32
    src/main.cpp
    src/app.cpp
    src/app_screens.cpp
    src/ui.cpp
    src/util.cpp
    src/subprocess.cpp
    src/gdf.cpp
    src/game_finder.cpp
    src/toolchain.cpp
    src/setup_steps.cpp
    src/game_config.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/embedded_files.cpp")
set_target_properties(optroy_launcher PROPERTIES OUTPUT_NAME "Operation Troy")
target_link_libraries(optroy_launcher PRIVATE launcher_imgui SDL3::SDL3-static)
target_compile_definitions(optroy_launcher PRIVATE
    OPTROY_LAUNCHER_VERSION="${PROJECT_VERSION}"
    OPTROY_REPO_OWNER="Hatdagger"
    OPTROY_REPO_NAME="Mobile-Suit-Gundam-Operation-Troy")
if(OPTROY_LAUNCHER_HEADLESS)
    target_compile_definitions(optroy_launcher PRIVATE OPTROY_HEADLESS=1)
endif()
if(WIN32)
    target_compile_definitions(optroy_launcher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(optroy_launcher PRIVATE shell32 ole32)
endif()
if(MSVC)
    target_compile_options(optroy_launcher PRIVATE /W3 /utf-8)
    target_compile_options(launcher_imgui PRIVATE /utf-8)
else()
    target_compile_options(optroy_launcher PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

if(OPTROY_LAUNCHER_TOOL)
    add_executable(optroy_launcher_tool tests/tool.cpp src/util.cpp src/subprocess.cpp src/gdf.cpp src/game_finder.cpp
                   src/toolchain.cpp src/setup_steps.cpp "${CMAKE_CURRENT_BINARY_DIR}/embedded_files.cpp")
    target_include_directories(optroy_launcher_tool PRIVATE src)
    target_compile_definitions(optroy_launcher_tool PRIVATE OPTROY_REPO_OWNER="Hatdagger"
                               OPTROY_REPO_NAME="Mobile-Suit-Gundam-Operation-Troy")
    target_link_libraries(optroy_launcher_tool PRIVATE SDL3::SDL3-static)
endif()
