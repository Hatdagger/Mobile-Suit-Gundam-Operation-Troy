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

rexglue_setup_target(gundam_operation_troy)
