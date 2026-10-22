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
    src/gpu_info.cpp
    src/vulkan_functions.cpp
)

target_include_directories(pkmn_render
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(pkmn_render PRIVATE pkmn_warnings)

if(PKMNG5PC_RENDER_VULKAN)
    pkmng5pc_ensure_vulkan_headers()
    target_sources(pkmn_render PRIVATE src/vulkan_context.cpp src/vulkan_presenter.cpp)
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

if(PKMNG5PC_NATIVE_RENDERER)
    if(NOT PKMNG5PC_RENDER_VULKAN)
        message(FATAL_ERROR
            "PKMNG5PC_NATIVE_RENDERER=ON requires PKMNG5PC_RENDER_VULKAN=ON.")
    endif()

    include(Shaders)
    pkmng5pc_ensure_glslang()

    set(PKMNG5PC_NDSRECOMP_ROOT "${CMAKE_SOURCE_DIR}/third_party/ndsrecomp")
    set(PKMNG5PC_MELONDS_VENDOR "${PKMNG5PC_NDSRECOMP_ROOT}/runner/vendor/melonds")

    add_executable(pkmn_ds_shader_gen "${CMAKE_SOURCE_DIR}/tools/ds_shader_gen.cpp")
    target_include_directories(pkmn_ds_shader_gen PRIVATE "${PKMNG5PC_MELONDS_VENDOR}")
    target_link_libraries(pkmn_ds_shader_gen PRIVATE pkmn_warnings)

    set(_ds_shader_spec "${CMAKE_CURRENT_SOURCE_DIR}/src/ds/ds_shaders.txt")
    set(_ds_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/ds_shaders")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ds_shader_spec}")

    file(STRINGS "${_ds_shader_spec}" _ds_shader_lines)
    set(_ds_shader_names "")
    foreach(_line IN LISTS _ds_shader_lines)
        string(STRIP "${_line}" _line)
        if(_line STREQUAL "" OR _line MATCHES "^#")
            continue()
        endif()
        string(REPLACE "|" ";" _fields "${_line}")
        list(GET _fields 0 _name)
        list(APPEND _ds_shader_names "${_name}")
    endforeach()
    if(NOT _ds_shader_names)
        message(FATAL_ERROR "no shaders listed in ${_ds_shader_spec}")
    endif()

    file(MAKE_DIRECTORY "${_ds_shader_dir}")
    set(_ds_shader_header "${_ds_shader_dir}/ds_shader_ids.h")
    set(_ds_header_text "// Generated from render/src/ds/ds_shaders.txt. Do not edit.\n")
    string(APPEND _ds_header_text "#pragma once\n\n")
    string(APPEND _ds_header_text "#define PKMN_DS_SHADER_LIST(X) \\\n")
    foreach(_name IN LISTS _ds_shader_names)
        string(APPEND _ds_header_text "    X(${_name}) \\\n")
    endforeach()
    string(APPEND _ds_header_text
        "\n"
        "namespace pkmn::render::ds {\n"
        "enum class ShaderId : unsigned {\n"
        "#define PKMN_DS_SHADER_ENUM(name) name,\n"
        "    PKMN_DS_SHADER_LIST(PKMN_DS_SHADER_ENUM)\n"
        "#undef PKMN_DS_SHADER_ENUM\n"
        "    Count,\n"
        "};\n"
        "constexpr unsigned kNumShaders = static_cast<unsigned>(ShaderId::Count);\n"
        "constexpr unsigned shader_index(ShaderId id)\n"
        "{\n"
        "    return static_cast<unsigned>(id);\n"
        "}\n"
        "} // namespace pkmn::render::ds\n")
    file(WRITE "${_ds_shader_header}" "${_ds_header_text}")

    set(_ds_generated_sources "")
    foreach(_name IN LISTS _ds_shader_names)
        list(APPEND _ds_generated_sources "${_ds_shader_dir}/${_name}.comp")
    endforeach()

    add_custom_command(
        OUTPUT ${_ds_generated_sources}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_ds_shader_dir}"
        COMMAND $<TARGET_FILE:pkmn_ds_shader_gen> "${_ds_shader_dir}" "${_ds_shader_spec}"
        DEPENDS pkmn_ds_shader_gen "${_ds_shader_spec}"
                "${PKMNG5PC_MELONDS_VENDOR}/GPU3D_Compute_shaders.h"
        COMMENT "Generating native DS Vulkan compute shaders"
        VERBATIM)
    set_source_files_properties(${_ds_generated_sources} PROPERTIES GENERATED TRUE)

    add_library(pkmn_render_ds STATIC
        src/ds/ds_3d.cpp
        src/ds/runner_bridge.cpp
        "${PKMNG5PC_MELONDS_VENDOR}/GPU3D_Texcache.cpp"
        "${PKMNG5PC_MELONDS_VENDOR}/xxhash/xxhash.c")
    # The top-level project only enables CXX; xxhash is C but compiles as C++
    # here so its symbols match GPU3D_Texcache.cpp's declarations.
    set_source_files_properties("${PKMNG5PC_MELONDS_VENDOR}/xxhash/xxhash.c"
        PROPERTIES LANGUAGE CXX)
    target_include_directories(pkmn_render_ds PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        "${_ds_shader_dir}"
        "${PKMNG5PC_MELONDS_VENDOR}"
        "${PKMNG5PC_NDSRECOMP_ROOT}/runner"
        "${PKMNG5PC_NDSRECOMP_ROOT}/runner/src")
    target_compile_definitions(pkmn_render_ds PRIVATE
        PKMN_RENDER_DS=1
        PKMNG5PC_NATIVE_RENDERER=1
        VK_NO_PROTOTYPES=1)
    target_link_libraries(pkmn_render_ds
        PUBLIC pkmn::render
        PRIVATE Vulkan::Headers pkmn_warnings)

    set(PKMNG5PC_SHADER_SOURCES "")
    foreach(_name IN LISTS _ds_shader_names)
        pkmng5pc_add_shader(
            SOURCE "${_ds_shader_dir}/${_name}.comp"
            STAGE comp
            SYMBOL "pkmng5pc_${_name}_spv")
    endforeach()
    target_sources(pkmn_render_ds PRIVATE ${PKMNG5PC_SHADER_SOURCES})

    add_library(pkmn::render_ds ALIAS pkmn_render_ds)
endif()

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
