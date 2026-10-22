# Build-time GLSL -> SPIR-V, with the result embedded as a C++ array.
#
# glslang is pinned and fetched like the other host dependencies. Only the
# standalone validator is built; optimisation and the test/HLSL/JS frontends
# are off. No SPIR-V blob is committed: the generated source lives in the build
# tree and is compiled into the target.

include_guard(GLOBAL)

include(FetchContent)

function(pkmng5pc_ensure_glslang)
    if(TARGET glslang-standalone)
        set(PKMNG5PC_GLSLANG_TARGET glslang-standalone PARENT_SCOPE)
        return()
    elseif(TARGET glslangValidator)
        set(PKMNG5PC_GLSLANG_TARGET glslangValidator PARENT_SCOPE)
        return()
    endif()

    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_JS OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_WEBMIN OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_WEBMIN_DEPS OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(glslang
        GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
        GIT_TAG vulkan-sdk-1.3.296.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glslang)

    if(TARGET glslang-standalone)
        set(PKMNG5PC_GLSLANG_TARGET glslang-standalone PARENT_SCOPE)
    elseif(TARGET glslangValidator)
        set(PKMNG5PC_GLSLANG_TARGET glslangValidator PARENT_SCOPE)
    else()
        message(FATAL_ERROR
            "glslang was fetched but neither glslang-standalone nor "
            "glslangValidator was created.")
    endif()
endfunction()

# pkmng5pc_add_shader(SOURCE <file> STAGE <stage> SYMBOL <identifier>)
#
# Appends the generated C++ source to PKMNG5PC_SHADER_SOURCES in the caller's
# scope. The generated file defines
# `alignas(4) extern const unsigned char <SYMBOL>[]` and
# `extern const std::size_t <SYMBOL>_size`.
function(pkmng5pc_add_shader)
    cmake_parse_arguments(ARG "" "SOURCE;STAGE;SYMBOL" "" ${ARGN})
    if(NOT ARG_SOURCE OR NOT ARG_SYMBOL)
        message(FATAL_ERROR "pkmng5pc_add_shader requires SOURCE and SYMBOL")
    endif()
    if(NOT ARG_STAGE)
        set(ARG_STAGE comp)
    endif()
    if(NOT ARG_SYMBOL MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "invalid shader symbol: ${ARG_SYMBOL}")
    endif()
    if(NOT PKMNG5PC_GLSLANG_TARGET)
        message(FATAL_ERROR "pkmng5pc_ensure_glslang() must run before adding shaders")
    endif()

    get_filename_component(_name "${ARG_SOURCE}" NAME_WE)
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    set(_spv "${_dir}/${_name}.spv")
    set(_cpp "${_dir}/${_name}_spv.cpp")

    add_custom_command(
        OUTPUT "${_spv}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
        COMMAND $<TARGET_FILE:${PKMNG5PC_GLSLANG_TARGET}>
            -V --target-env vulkan1.1 -S ${ARG_STAGE}
            -o "${_spv}" "${ARG_SOURCE}"
        DEPENDS "${ARG_SOURCE}" ${PKMNG5PC_GLSLANG_TARGET}
        VERBATIM)

    add_custom_command(
        OUTPUT "${_cpp}"
        COMMAND ${CMAKE_COMMAND}
            -DINPUT=${_spv}
            -DOUTPUT=${_cpp}
            -DSYMBOL=${ARG_SYMBOL}
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/embed_spirv.cmake"
        DEPENDS "${_spv}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/embed_spirv.cmake"
        VERBATIM)

    set_source_files_properties("${_cpp}" PROPERTIES GENERATED TRUE)
    set(PKMNG5PC_SHADER_SOURCES
        "${PKMNG5PC_SHADER_SOURCES};${_cpp}"
        PARENT_SCOPE)
endfunction()
