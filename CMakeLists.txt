cmake_minimum_required(VERSION 3.24)

project(pkmng5pc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(CompilerWarnings)

option(PKMNG5PC_BUILD_TESTS "Build unit tests" ON)
option(PKMNG5PC_BUILD_TOOLS "Build local extraction tools" ON)
option(PKMNG5PC_BUILD_RUNNER
    "Build the ndsrecomp runner and its SDL3 dependency (needs generated title banks)" OFF)
option(PKMNG5PC_BUILD_RENDER
    "Build the render seam and its GPU backends" ON)
option(PKMNG5PC_NATIVE_RENDERER
    "Build the native DS 3D/2D render sinks (alpha, runtime off by default)" OFF)

if(PKMNG5PC_NATIVE_RENDERER AND NOT PKMNG5PC_BUILD_RENDER)
    message(FATAL_ERROR
        "PKMNG5PC_NATIVE_RENDERER=ON requires PKMNG5PC_BUILD_RENDER=ON.")
endif()

add_subdirectory(rom)

if(PKMNG5PC_BUILD_RENDER)
    add_subdirectory(render)
endif()

if(PKMNG5PC_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(PKMNG5PC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(PKMNG5PC_BUILD_RUNNER)
    include(Runner)
endif()
