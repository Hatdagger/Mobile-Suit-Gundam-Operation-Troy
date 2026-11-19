# optroy_netcheck: tests the online-play client (HTTP + Xenia Web Services calls)
# without the game. Built from the same SDK source files the game uses.
#
#   git submodule update --init --depth 1 thirdparty/rexglue-sdk/thirdparty/fmt thirdparty/rexglue-sdk/thirdparty/rapidjson
#   cmake -S tools/netcheck -B out/netcheck
#   cmake --build out/netcheck --config Release
#
# Contains no game code or assets.

cmake_minimum_required(VERSION 3.25)
project(optroy_netcheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static C++ runtime on Windows, so the .exe runs on its own.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(SDK "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/rexglue-sdk")

foreach(lib fmt rapidjson)
    if(NOT EXISTS "${SDK}/thirdparty/${lib}/.git")
        message(FATAL_ERROR "Submodule thirdparty/rexglue-sdk/thirdparty/${lib} is missing. Run:\n"
            "  git submodule update --init --depth 1 thirdparty/rexglue-sdk/thirdparty/${lib}")
    endif()
endforeach()

add_subdirectory("${SDK}/thirdparty/fmt" fmt EXCLUDE_FROM_ALL)

if(WIN32)
    set(HTTP_SOURCE "${SDK}/src/core/http_win.cpp")
else()
    set(HTTP_SOURCE "${SDK}/src/core/http_curl.cpp")
endif()

add_executable(optroy_netcheck
    netcheck.cpp
    ${HTTP_SOURCE}
    "${SDK}/src/kernel/xam/live/live_api.cpp"
    "${SDK}/src/kernel/xam/live/live_identity.cpp"
    "${SDK}/src/kernel/xam/live/live_properties.cpp"
    "${SDK}/src/kernel/xam/live/live_upnp.cpp"
    "${SDK}/src/system/xsocket_native.cpp"
)
target_include_directories(optroy_netcheck PRIVATE "${SDK}/include")
target_include_directories(optroy_netcheck SYSTEM PRIVATE "${SDK}/thirdparty/rapidjson/include")
target_compile_definitions(optroy_netcheck PRIVATE RAPIDJSON_HAS_STDSTRING=1)
target_link_libraries(optroy_netcheck PRIVATE fmt::fmt)

if(WIN32)
    target_link_libraries(optroy_netcheck PRIVATE winhttp ws2_32)
else()
    target_link_libraries(optroy_netcheck PRIVATE ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    target_compile_options(optroy_netcheck PRIVATE /W4 /utf-8)
else()
    target_compile_options(optroy_netcheck PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()
