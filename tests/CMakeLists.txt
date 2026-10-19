add_executable(pkmn_tests
    main.cpp
    compression_test.cpp
    extract_test.cpp
    rom_overlay_test.cpp
    sha1_test.cpp
    sha256_test.cpp
)

target_link_libraries(pkmn_tests PRIVATE pkmn::core pkmn_warnings)

# The render seam is optional; its tests join the suite only when it is built.
# SDL3 is needed because the presenter factory reaches the Vulkan backend.
if(TARGET pkmn::render)
    target_sources(pkmn_tests PRIVATE render_test.cpp)
    target_link_libraries(pkmn_tests PRIVATE pkmn::render SDL3::SDL3)
endif()

target_compile_definitions(pkmn_tests PRIVATE
    PKMNG5PC_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME pkmn_tests COMMAND pkmn_tests)

set_tests_properties(pkmn_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
