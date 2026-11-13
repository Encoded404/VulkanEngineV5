# ── Shared scene components ─────────────────────────────────────────
# The two object components live in their own static library so the example and
# the device-free component test import one definition, and so the component
# test builds the same code the scene runs.
#
# SDL3 is linked here and not on the executable: SimpleControllerComponent's
# global module fragment includes SDL3's key/mouse/gamepad headers to name the
# key codes, and the engine's own SDL3 link is PRIVATE.
add_library(basic_scene_core STATIC)
target_compile_features(basic_scene_core PUBLIC cxx_std_23)
set_target_properties(basic_scene_core PROPERTIES CXX_MODULE_STD ON)
target_link_libraries(basic_scene_core PUBLIC ${ENGINE_TARGET})

find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(basic_scene_core PRIVATE SDL3::SDL3)

target_compile_options(basic_scene_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)
vkengine_target_config_flags(basic_scene_core)

target_sources(basic_scene_core PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    FILES
        Components/SimpleControllerComponent.cppm
        Components/TransformControlComponent.cppm
)

if(TARGET project_sanitizers)
    target_link_libraries(basic_scene_core PRIVATE project_sanitizers)
endif()
if(TARGET project_sections)
    target_link_libraries(basic_scene_core PRIVATE project_sections)
endif()
if(TARGET project_size)
    target_link_libraries(basic_scene_core PRIVATE project_size)
endif()

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(basic_scene_core)
endif()

# ── The example executable ──────────────────────────────────────────
add_engine_example(basic_scene
    APP_TITLE       "VulkanEngineV5 Basic Scene"
    MODULE_PREFIX   Examples.BasicScene
    SHADER_NAMESPACE BasicScene
    SOURCES
        main.cpp
        Game.cpp
    MODULES
        Game.cppm
    SHADERS
        NormalsFrag  normals.slang  fragment main
        SolidFrag    solid.slang    fragment main
    ASSET_DIRS
        models
        textures
)

target_link_libraries(basic_scene PRIVATE basic_scene_core)
