# Local, tracked modifications to the pinned ndsrecomp submodule
# (docs/adr/0006-ndsrecomp-native-present-hook.md,
# docs/adr/0007-native-ds-render-sinks.md). Applied at configure time and
# idempotently because the sources must carry the change before either the
# render sinks or the runner are configured.
#
# An already-applied patch is recognised with `git apply --check --reverse`; a
# patch that applies neither way means the submodule moved, so the message
# names the restore command instead of failing obscurely.
function(pkmng5pc_apply_ndsrecomp_patches)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/ndsrecomp/.git")
        message(FATAL_ERROR
            "third_party/ndsrecomp is not initialized; run:\n"
            "  pwsh tools/bootstrap_ndsrecomp.ps1   (Windows)\n"
            "  sh tools/bootstrap_ndsrecomp.sh     (Linux/macOS)\n")
    endif()
    file(GLOB _patches "${CMAKE_SOURCE_DIR}/patches/ndsrecomp/*.patch")
    list(SORT _patches)
    foreach(_patch IN LISTS _patches)
        get_filename_component(_name "${_patch}" NAME)
        execute_process(
            COMMAND git apply --check --reverse "${_patch}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/third_party/ndsrecomp"
            RESULT_VARIABLE _already_applied
            OUTPUT_QUIET ERROR_QUIET)
        if(_already_applied EQUAL 0)
            continue()
        endif()
        execute_process(
            COMMAND git apply --check "${_patch}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/third_party/ndsrecomp"
            RESULT_VARIABLE _applies
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT _applies EQUAL 0)
            message(FATAL_ERROR
                "patch ${_name} applies to third_party/ndsrecomp neither "
                "forwards nor backwards. The submodule is at a different "
                "revision or carries other local edits; restore it with:\n"
                "  git -C third_party/ndsrecomp checkout .\n")
        endif()
        execute_process(
            COMMAND git apply "${_patch}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/third_party/ndsrecomp"
            RESULT_VARIABLE _applied
            OUTPUT_QUIET ERROR_QUIET)
        if(NOT _applied EQUAL 0)
            message(FATAL_ERROR "failed to apply ${_name}")
        endif()
        message(STATUS "ndsrecomp: applied ${_name}")
    endforeach()
endfunction()
