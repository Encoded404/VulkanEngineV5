# ── Shared scene explorer ───────────────────────────────────────────
# One static library holding the reusable half of the explorer: camera modes,
# the declarative options panel, the scene catalog types and the application
# that wires them to the engine. Examples link this and supply a SceneCatalog.
add_library(explorer_core STATIC)
target_compile_features(explorer_core PUBLIC cxx_std_23)
set_target_properties(explorer_core PROPERTIES CXX_MODULE_STD ON)
target_link_libraries(explorer_core PUBLIC ${ENGINE_TARGET})

# SDL3: this library reads key codes and mouse buttons from the input system's
# raw state. ImGui: OptionsUi and ExplorerApp own the panels. Both are PRIVATE;
# consumers only see the exported C++ interfaces.
find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
target_link_libraries(explorer_core PRIVATE SDL3::SDL3 imgui::imgui CURL::libcurl)

target_compile_options(explorer_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)
vkengine_target_config_flags(explorer_core)

target_sources(explorer_core PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    FILES
        Math.cppm
        Options.cppm
        Download.cppm
        Scene.cppm
        Camera.cppm
        App.cppm
)
target_sources(explorer_core PRIVATE
    Math.cpp
    Options.cpp
    Download.cpp
    Scene.cpp
    Camera.cpp
    App.cpp
)

if(TARGET project_sanitizers)
    target_link_libraries(explorer_core PRIVATE project_sanitizers)
endif()

if(TARGET project_sections)
    target_link_libraries(explorer_core PRIVATE project_sections)
endif()
if(TARGET project_size)
    target_link_libraries(explorer_core PRIVATE project_size)
endif()

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(explorer_core)
endif()
