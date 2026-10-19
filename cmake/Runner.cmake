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
