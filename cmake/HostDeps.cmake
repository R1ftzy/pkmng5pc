# Host libraries shared by the renderer and the vendored runner superbuild.
#
# ndsrecomp's runner resolves SDL3 with `find_package(SDL3 CONFIG REQUIRED)` and
# looks nowhere else, so SDL3 must exist as a target (and via the config shim)
# before the runner subdirectory is added. The renderer needs the same library.
# These helpers are idempotent, so whichever subsystem is configured first
# performs the fetch and the other joins it.

include_guard(GLOBAL)

include(FetchContent)

function(pkmng5pc_ensure_sdl3)
    if(TARGET SDL3::SDL3)
        return()
    endif()

    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)

    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.18
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)

    # The runner's `find_package(SDL3 CONFIG REQUIRED)` is satisfied by the
    # shim, which re-exports the FetchContent target as SDL3::SDL3.
    set(SDL3_DIR "${CMAKE_SOURCE_DIR}/cmake/sdl3-shim" CACHE PATH
        "Config-package shim for ndsrecomp's find_package(SDL3 CONFIG REQUIRED)" FORCE)
endfunction()

# Khronos Vulkan headers (Apache-2.0). The build needs only the headers: the
# loader ships with the GPU driver and every entry point is resolved at run
# time (see render/src/vulkan_functions.h). A local Vulkan SDK is preferred
# when present; otherwise the headers are fetched, matching the SDL3 fetch.
function(pkmng5pc_ensure_vulkan_headers)
    if(TARGET Vulkan::Headers)
        return()
    endif()

    find_package(Vulkan QUIET)
    if(TARGET Vulkan::Headers)
        return()
    endif()

    FetchContent_Declare(VulkanHeaders
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.3.302
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(VulkanHeaders)
endfunction()
