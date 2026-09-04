# Minimal starting template: one mesh (engine fallback quad), one custom
# fragment shader, no external assets. Copy this directory to start a new
# app/example and rename the target, MODULE_PREFIX and SHADER_NAMESPACE.
add_engine_example(minimal
    APP_TITLE        "VulkanEngineV5 Minimal"
    MODULE_PREFIX    Examples.Minimal
    SHADER_NAMESPACE Minimal
    SOURCES
        main.cpp
        Game.cpp
    MODULES
        Game.cppm
    SHADERS
        CheckerFrag  checker.slang  fragment  main
)
