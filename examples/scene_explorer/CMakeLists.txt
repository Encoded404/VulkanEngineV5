# The explorer example: a scene catalog on top of the shared explorer library.
# It ships no assets; point --assets at a directory of glTF files (CC0 / CC BY
# 4.0 only) or use the procedural Material Lab.
#
# The catalog and credits are generated from catalog.json on every configure
# (header) and every build (credits folder next to the executable), so there is
# no manual step to forget and no second copy of the asset metadata.
add_engine_example(scene_explorer
    APP_TITLE       "VulkanEngineV5 Scene Explorer"
    MODULE_PREFIX   Examples.SceneExplorer
    SOURCES
        main.cpp
    LICENSE_CATALOG
        "${CMAKE_CURRENT_SOURCE_DIR}/catalog.json"
)

# ── The scene catalog ───────────────────────────────────────────────
# Its own static library, like the other examples, so the device-free tests
# import the same BuildCatalog/BuildDownloadCatalog/FilterCatalog the executable
# runs. Scenes.cppm is registered here and must not be listed again on the
# executable: one source in two CXX_MODULES file sets is a configure error.
add_library(scene_explorer_core STATIC)
target_compile_features(scene_explorer_core PUBLIC cxx_std_23)
set_target_properties(scene_explorer_core PROPERTIES CXX_MODULE_STD ON)
target_link_libraries(scene_explorer_core PUBLIC ${ENGINE_TARGET} explorer_core)

# The generated catalog header is included in Scenes.cpp's global module
# fragment, so it needs no module plumbing, only this include path.
target_include_directories(scene_explorer_core PRIVATE
    "${CMAKE_BINARY_DIR}/generated/catalog")

target_compile_options(scene_explorer_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)
vkengine_target_config_flags(scene_explorer_core)

target_sources(scene_explorer_core PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
    FILES
        Scenes.cppm
)
target_sources(scene_explorer_core PRIVATE
    Scenes.cpp
)

if(TARGET project_sanitizers)
    target_link_libraries(scene_explorer_core PRIVATE project_sanitizers)
endif()

if(TARGET project_sections)
    target_link_libraries(scene_explorer_core PRIVATE project_sections)
endif()

if(TARGET project_size)
    target_link_libraries(scene_explorer_core PRIVATE project_size)
endif()

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(scene_explorer_core)
endif()

target_link_libraries(scene_explorer PRIVATE explorer_core scene_explorer_core)
