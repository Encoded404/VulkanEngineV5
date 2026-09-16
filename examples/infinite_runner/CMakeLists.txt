# Infinite runner example: a cube weaves through gaps in oncoming walls.
# Geometry and colours are generated at runtime (procedural cube + solid
# colour textures), so no asset files are needed. It uses the engine's built-in
# standard PBR mesh shader (no per-example SHADERS).
add_engine_example(infinite_runner
    APP_TITLE        "VulkanEngineV5 Infinite Runner"
    MODULE_PREFIX    Examples.InfiniteRunner
    SOURCES
        main.cpp
        Game.cpp
    MODULES
        Game.cppm
        Sweep.cppm
        Wall.cppm
)
