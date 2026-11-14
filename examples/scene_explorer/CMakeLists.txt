# The explorer example: a scene catalog on top of the shared explorer library.
# It ships no assets; point --assets at a directory of glTF files (CC0 / CC BY
# 4.0 only) or use the procedural Material Lab.
add_engine_example(scene_explorer
    APP_TITLE       "VulkanEngineV5 Scene Explorer"
    MODULE_PREFIX   Examples.SceneExplorer
    SOURCES
        main.cpp
        Scenes.cpp
    MODULES
        Scenes.cppm
)

target_link_libraries(scene_explorer PRIVATE explorer_core)
