# The render seam (`AGENTS.md` §5, `Plan.md` M8).
#
# `pkmn_render` is the only place that names a GPU API. Vulkan is the first
# backend (docs/adr/0005-vulkan-first-renderer.md); when it is unavailable or
# disabled the factory degrades to the Null presenter so the rest of the port
# still builds and runs.

include(HostDeps)

option(PKMNG5PC_RENDER_VULKAN "Build the Vulkan renderer backend" ON)

add_library(pkmn_render STATIC
    src/backend.cpp
    src/compose.cpp
    src/vulkan_functions.cpp
)

target_include_directories(pkmn_render
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(pkmn_render PRIVATE pkmn_warnings)

if(PKMNG5PC_RENDER_VULKAN)
    pkmng5pc_ensure_vulkan_headers()
    target_sources(pkmn_render PRIVATE src/vulkan_presenter.cpp)
    target_compile_definitions(pkmn_render PRIVATE
        PKMN_RENDER_VULKAN=1
        # Resolve every Vulkan entry point at run time; no import library and
        # no SDK are required to link.
        VK_NO_PROTOTYPES=1
    )
    target_link_libraries(pkmn_render PRIVATE Vulkan::Headers)
else()
    target_sources(pkmn_render PRIVATE src/vulkan_presenter_stub.cpp)
endif()

pkmng5pc_ensure_sdl3()
target_link_libraries(pkmn_render PRIVATE SDL3::SDL3)

# Bridge that lets the runner's frontend present through this seam
# (`render/include/pkmn/render/native_present.h`). It is a separate target
# because it is runner-specific: the seam library above neither knows nor cares
# who calls it. `cmake/Runner.cmake` links this into `nds_runner`.
add_library(pkmn_render_runner_bridge STATIC src/native_present.cpp)
target_link_libraries(pkmn_render_runner_bridge
    PUBLIC pkmn::render
    PRIVATE SDL3::SDL3 pkmn_warnings)

add_executable(pkmn_render_smoke src/smoke_main.cpp)
target_link_libraries(pkmn_render_smoke PRIVATE pkmn_render pkmn_warnings SDL3::SDL3)

add_library(pkmn::render ALIAS pkmn_render)
add_library(pkmn::render_runner_bridge ALIAS pkmn_render_runner_bridge)
