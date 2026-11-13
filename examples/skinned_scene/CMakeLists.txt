# Minimal skeletal-animation example.
#
# One two-bone GLB and a SkeletonPose driven by a sine wave. No custom shaders:
# the engine's standard mesh vertex/fragment shaders render the deformed
# vertices, so the example exercises the builtin skinning modifier end to end.
add_engine_example(skinned_scene
    APP_TITLE        "VulkanEngineV5 Skinned Scene"
    MODULE_PREFIX    Examples.SkinnedScene
    SHADER_NAMESPACE SkinnedScene
    SOURCES
        main.cpp
        Game.cpp
    MODULES
        Game.cppm
    ASSET_DIRS
        models
)
