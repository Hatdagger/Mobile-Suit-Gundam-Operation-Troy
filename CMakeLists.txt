# gundam_operation_troy - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(gundam_operation_troy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(GUNDAMOPERATIONTROY_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(gundam_operation_troy WIN32 ${GUNDAMOPERATIONTROY_SOURCES})
else()
    add_executable(gundam_operation_troy ${GUNDAMOPERATIONTROY_SOURCES})
endif()

# GPU_PLUGINS xenos builds the SDK's Xenos GPU emulation (rexgpu-xenos.dll) and
# copies it next to the game; the game loads it at start (see OnPreSetup).
rexglue_setup_target(gundam_operation_troy GPU_PLUGINS xenos)
