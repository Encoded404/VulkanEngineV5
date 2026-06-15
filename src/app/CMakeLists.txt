add_executable(${APP_TARGET})
target_sources(${APP_TARGET} PRIVATE
    main.cpp
    Game.cpp
)

# Compile slang shaders to SPIR-V + generate C++20 modules with #embed
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/external/SlangSpriVCompilerHelper/cmake")
include(SlangSpirVCompiler)
set(_shader_gen_dir "${CMAKE_BINARY_DIR}/generated/shaders/app")
set(_shader_opt_level
    "$<$<CONFIG:Debug>:0>$<$<CONFIG:RelWithDebInfo>:2>$<$<CONFIG:Release>:3>")
add_slang_shaders(
    TARGET      ${APP_TARGET}_shaders
    OUTPUT_DIR  ${_shader_gen_dir}
    NAMESPACE   App
    SHADER_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    COMPILER    slang-spirv-compiler
    OPT_LEVEL   ${_shader_opt_level}
    SHADERS
        NormalsFrag  normals.slang  fragment main
        SolidFrag    solid.slang    fragment main
)
add_dependencies(${APP_TARGET} ${APP_TARGET}_shaders)
get_target_property(_gen_shader_mods ${APP_TARGET}_shaders SLANG_CPPM_FILES)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_sources(${APP_TARGET} PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            Components/SimpleControllerComponent.cppm
            Components/TransformControlComponent.cppm
            Game.cppm
    )

    # Register generated slang shader module files
    if(_gen_shader_mods)
        target_sources(${APP_TARGET} PUBLIC
            FILE_SET CXX_MODULES
            BASE_DIRS ${_shader_gen_dir}
            FILES ${_gen_shader_mods}
        )
    endif()
endif()

target_link_libraries(${APP_TARGET} PRIVATE ${ENGINE_TARGET} ${BACKEND_TARGET} slang_shared_reflection)

find_package(SDL3 CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

target_link_libraries(${APP_TARGET} PRIVATE
    SDL3::SDL3
    CLI11::CLI11
    Vulkan::HppModule
)

set_target_properties(${APP_TARGET} PROPERTIES OUTPUT_NAME ${APP_TARGET})

if(SYSTEM_VULKAN)
  set_target_properties(${APP_TARGET} PROPERTIES BUILD_RPATH "/usr/lib64")
endif()

if(OUTPUT_PATH)
    set_target_properties(${APP_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_PATH})
endif()

target_compile_features(${APP_TARGET} PRIVATE cxx_std_20)

target_compile_options(${APP_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)

if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${APP_TARGET} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

target_compile_definitions(${APP_TARGET}
    PUBLIC
        $<$<BOOL:${ENABLE_LOGGING}>:LOGIFACE_ENABLE_LOGGING=1>
        $<$<NOT:$<BOOL:${ENABLE_LOGGING}>>:LOGIFACE_ENABLE_LOGGING=0>
        LOGIFACE_DEFAULT_TO_FUNCTION=1
)

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(${APP_TARGET})
endif()

include(CopyAppAssets)
configure_app_asset_copy(${APP_TARGET}
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/models"
    DEST_SUBDIR "models"
)
configure_app_asset_copy(${APP_TARGET}
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/textures"
    DEST_SUBDIR "textures"
)