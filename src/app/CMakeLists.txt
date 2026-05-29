add_executable(${APP_TARGET})
target_sources(${APP_TARGET} PRIVATE
    main.cpp
    Game.cpp
)

include(ShaderUtils)
compile_shaders(${APP_TARGET}_shaders SUBDIR "app")

# Generate C++ module files with #embed for each compiled shader
include(ShaderModuleGen)
set(_shader_gen_dir "${CMAKE_BINARY_DIR}/shaders/app")
generate_shader_modules(${APP_TARGET}_shader_modules
    NAMESPACE "App"
    OUTPUT_DIR ${_shader_gen_dir}
)
add_dependencies(${APP_TARGET} ${APP_TARGET}_shader_modules)
get_target_property(_gen_shader_mods ${APP_TARGET}_shader_modules GENERATED_SHADER_MODULES)

# #embed is a Clang extension in C++20 mode; suppress the warning for generated shader modules
if(_gen_shader_mods)
    set_source_files_properties(${_gen_shader_mods} PROPERTIES
        COMPILE_OPTIONS "-Wno-c23-extensions"
    )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_sources(${APP_TARGET} PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            Components/SimpleControllerComponent.cppm
            Components/TransformControlComponent.cppm
            Game.cppm
    )

    # Register generated shader module files
    if(_gen_shader_mods)
        target_sources(${APP_TARGET} PUBLIC
            FILE_SET CXX_MODULES
            BASE_DIRS ${_shader_gen_dir}
            FILES ${_gen_shader_mods}
        )
    endif()
endif()

target_link_libraries(${APP_TARGET} PRIVATE ${ENGINE_TARGET} ${BACKEND_TARGET})

find_package(SDL3 CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS stacktrace_basic)
find_package(CLI11 CONFIG REQUIRED)

target_link_libraries(${APP_TARGET} PRIVATE
    SDL3::SDL3
    Boost::stacktrace_basic
    CLI11::CLI11
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
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

target_precompile_headers(${APP_TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}/cmake/pch_vulkan.hpp
    ${CMAKE_SOURCE_DIR}/cmake/pch_core.hpp
)

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

get_target_property(_engine_shader_dir ${ENGINE_TARGET}_shaders SHADER_OUTPUT_DIR)
get_target_property(_app_shader_dir ${APP_TARGET}_shaders SHADER_OUTPUT_DIR)
add_custom_command(TARGET ${APP_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${_app_shader_dir} ${_engine_shader_dir}
    COMMENT "Copying app shaders to engine shader output directory"
)
