# ndsrecomp integration.
#
# ndsrecomp ships no root CMakeLists: `runner/` and `recompiler/` are separate
# standalone projects. Its runner resolves SDL3 with
# `find_package(SDL3 CONFIG REQUIRED)` and looks nowhere else, so this project's
# CMake has to be the superbuild that provides SDL3 *before* adding the runner
# subdirectory.
#
# Title banks must exist at configure time: the runner globs them during
# configure and treats an empty bank group as a fatal error. Generate them with
# `pkmn_bankgen` first (see README.md).

set(PKMNG5PC_BANKS_DIR "${CMAKE_SOURCE_DIR}/banks/black" CACHE PATH
    "Directory of generated title banks (produced by pkmn_bankgen)")

set(PKMNG5PC_TITLE_ROM_SHA1 "26ad0b9967aa279c4a266ee69f52b9b2332399a5" CACHE STRING
    "Lowercase SHA-1 of the ROM the generated banks belong to")

file(GLOB _pkmng5pc_bank_bodies "${PKMNG5PC_BANKS_DIR}/*_[0-9][0-9].c")
if(NOT _pkmng5pc_bank_bodies)
    message(FATAL_ERROR
        "PKMNG5PC_BUILD_RUNNER=ON but no generated title banks were found in:\n"
        "  ${PKMNG5PC_BANKS_DIR}\n"
        "Generate them first, e.g.:\n"
        "  pkmn_bankgen \"roms/<your rom>.nds\" banks/black \\\n"
        "      --recompiler third_party/ndsrecomp/recompiler/build/nds_recompile\n")
endif()

# SDL3 is shared with the renderer; the helper fetches it once and points
# find_package at the config shim (see cmake/HostDeps.cmake).
include(HostDeps)
pkmng5pc_ensure_sdl3()

# Present the two DS screens through the port's own seam (`render/`) instead of
# the runner's SDL blit. Opt-in at run time via `tools/play -Native`, which sets
# PKMNG5PC_PRESENT=native; a build with this OFF has no such path at all.
if(PKMNG5PC_NATIVE_PRESENT)
    if(NOT TARGET pkmn::render_runner_bridge)
        message(FATAL_ERROR
            "PKMNG5PC_NATIVE_PRESENT=ON requires PKMNG5PC_BUILD_RENDER=ON.")
    endif()
endif()

# ndsrecomp options. Cache-set with FORCE so they win over the defaults their
# CMakeLists declares.
#
# NDS_BOOTSTRAP_FIRMWARE=ON builds with BIOS banks only: we have no retail
# firmware.bin to generate firmware banks from, and the boot path uses
# `--boot direct --generated-firmware`, which synthesizes a firmware image at
# run time instead.
set(NDS_BOOTSTRAP_FIRMWARE ON CACHE BOOL "" FORCE)
set(NDS_SDL_BACKEND "SDL3" CACHE STRING "" FORCE)
set(NDS_ENABLE_COMPUTE_RENDERER OFF CACHE BOOL "" FORCE)
set(NDS_TITLE_BANK_DIR "${PKMNG5PC_BANKS_DIR}" CACHE PATH "" FORCE)
set(NDS_TITLE_ROM_SHA1 "${PKMNG5PC_TITLE_ROM_SHA1}" CACHE STRING "" FORCE)

add_subdirectory(third_party/ndsrecomp/runner)

if(WIN32)
    target_compile_definitions(nds_runner PRIVATE NOMINMAX)
endif()

if(PKMNG5PC_NATIVE_PRESENT)
    # The hook compiled into the patched frontend.cpp and the bridge that
    # defines its entry points are one unit: the macro comes on only here, so a
    # standalone ndsrecomp build of the same patched tree is unaffected.
    target_compile_definitions(nds_runner PRIVATE PKMNG5PC_NATIVE_PRESENT=1)
    target_link_libraries(nds_runner PRIVATE pkmn::render_runner_bridge)
endif()

if(PKMNG5PC_NATIVE_RENDERER)
    if(NOT TARGET pkmn::render_ds)
        message(FATAL_ERROR
            "PKMNG5PC_NATIVE_RENDERER=ON requires the pkmn_render_ds target.")
    endif()
    target_compile_definitions(nds_runner PRIVATE PKMNG5PC_NATIVE_RENDERER=1)
    target_link_libraries(nds_runner PRIVATE pkmn::render_ds)
endif()
