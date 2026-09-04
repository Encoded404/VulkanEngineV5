include_guard(GLOBAL)

#[=======================================================================
# add_engine_example
#
#   add_engine_example(<name>
#     [APP_TITLE <title>]            # window/CLI app title (default: <name>)
#     [MODULE_PREFIX <prefix>]       # C++ module prefix (default: Examples.<name>)
#     [SHADER_NAMESPACE <ns>]        # shader namespace segment (default: <name>)
#     SOURCES <file...>              # .cpp implementation files
#     MODULES <file...>              # .cppm interface units (relative to example dir)
#     SHADERS <stem> <file> <stage> <entry> ...
#     ASSET_DIRS <dir...>            # asset folders copied next to the exe
#   )
#
# Creates a fully-configured example executable target that links the engine
# via EngineRuntime. Each example is self-contained:
#   - output goes to bin/examples/<Config>/<name>/
#   - engine + example shaders are deployed to <exe_dir>/shaders
#   - asset dirs are copied to <exe_dir>/<dir>
#   - modules are C++23 std-module based (Clang)
#   - sanitizers/sections glue and clang-tidy are applied like the engine libs
#=======================================================================]
function(add_engine_example NAME)
    cmake_parse_arguments(PARSE_ARGV 1 AEG
        ""
        "MODULE_PREFIX;SHADER_NAMESPACE;APP_TITLE"
        "SOURCES;MODULES;SHADERS;ASSET_DIRS"
    )

    if(NOT AEG_MODULE_PREFIX)
        set(AEG_MODULE_PREFIX "Examples.${NAME}")
    endif()
    if(NOT AEG_SHADER_NAMESPACE)
        set(AEG_SHADER_NAMESPACE "${NAME}")
    endif()
    if(NOT AEG_APP_TITLE)
        set(AEG_APP_TITLE "${NAME}")
    endif()

    add_executable(${NAME} ${AEG_SOURCES})

    # Every example lands in bin/examples/<Config>/<name>/ — the shader and
    # asset deploys below follow automatically via $<TARGET_FILE_DIR>.
    if(OUTPUT_PATH)
        set_target_properties(${NAME} PROPERTIES
            OUTPUT_NAME ${NAME}
            RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_PATH}/examples/$<CONFIG>/${NAME}"
        )
    endif()

    target_link_libraries(${NAME} PRIVATE
        ${RUNTIME_TARGET}
        slang_shared_reflection
    )

    find_package(SDL3 CONFIG REQUIRED)
    find_package(CLI11 CONFIG REQUIRED)
    target_link_libraries(${NAME} PRIVATE
        SDL3::SDL3
        CLI11::CLI11
        Vulkan::HppModule
    )

    # ---- compile example slang shaders to SPIR-V + C++20 modules ----
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/external/SlangSpriVCompilerHelper/cmake")
    include(SlangSpirVCompiler)
    set(_shader_gen_dir "${CMAKE_BINARY_DIR}/generated/shaders/examples/${NAME}")
    set(_shader_opt_level
        "$<$<CONFIG:Debug>:0>$<$<CONFIG:RelWithDebInfo>:2>$<$<CONFIG:Release>:3>")
    add_slang_shaders(
        TARGET      ${NAME}_shaders
        OUTPUT_DIR  ${_shader_gen_dir}
        NAMESPACE   ${AEG_SHADER_NAMESPACE}
        SHADER_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        COMPILER    slang-spirv-compiler
        OPT_LEVEL   ${_shader_opt_level}
        SHADERS
            ${AEG_SHADERS}
    )
    add_dependencies(${NAME} ${NAME}_shaders)

    # Deploy this example's shaders next to the executable.
    add_custom_command(
        TARGET ${NAME}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${NAME}>/shaders"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_shader_gen_dir}/*.spv" "$<TARGET_FILE_DIR:${NAME}>/shaders"
        COMMENT "Deploying ${NAME} SPIR-V shaders"
    )

    # Deploy the engine's shared shader set next to the executable too, so the
    # example is runnable on its own.
    get_property(_engine_shader_dir GLOBAL PROPERTY VKENGINE_ENGINE_SHADER_DIR)
    if(_engine_shader_dir)
        add_custom_command(
            TARGET ${NAME}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${NAME}>/shaders"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_engine_shader_dir}/*.spv" "$<TARGET_FILE_DIR:${NAME}>/shaders"
            COMMENT "Deploying engine SPIR-V shaders to ${NAME}"
        )
    endif()

    # ---- C++ module registration ----
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_sources(${NAME} PUBLIC
            FILE_SET CXX_MODULES
            BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
            FILES ${AEG_MODULES}
        )

        # Register generated slang shader module files
        get_target_property(_gen_shader_mods ${NAME}_shaders SLANG_CPPM_FILES)
        if(_gen_shader_mods)
            target_sources(${NAME} PUBLIC
                FILE_SET CXX_MODULES
                BASE_DIRS ${_shader_gen_dir}
                FILES ${_gen_shader_mods}
            )
        endif()
    endif()

    target_compile_features(${NAME} PRIVATE cxx_std_20)

    target_compile_definitions(${NAME}
        PUBLIC
            $<$<BOOL:${ENABLE_LOGGING}>:LOGIFACE_ENABLE_LOGGING=1>
            $<$<NOT:$<BOOL:${ENABLE_LOGGING}>>:LOGIFACE_ENABLE_LOGGING=0>
            LOGIFACE_DEFAULT_TO_FUNCTION=1
    )

    target_compile_definitions(${NAME} PRIVATE
        VKENGINE_SLANG_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/shaders/"
    )

    target_compile_options(${NAME} PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
        $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )

    if(CMAKE_CONFIGURATION_TYPES)
        target_compile_options(${NAME} PRIVATE
            "$<$<CONFIG:Debug>:-g>"
            "$<$<CONFIG:Debug>:-O0>"
            "$<$<CONFIG:RelWithDebInfo>:-g>"
            "$<$<CONFIG:RelWithDebInfo>:-O2>"
            "$<$<CONFIG:Release>:-O3>"
        )
    endif()

    if(TARGET project_sanitizers)
        target_link_libraries(${NAME} PRIVATE project_sanitizers)
    endif()
    if(TARGET project_sections)
        target_link_libraries(${NAME} PRIVATE project_sections)
    endif()

    if(COMMAND enable_target_clang_tidy)
        enable_target_clang_tidy(${NAME})
    endif()

    # ---- asset copies ----
    foreach(_asset_dir IN LISTS AEG_ASSET_DIRS)
        include(CopyAppAssets)
        configure_app_asset_copy(${NAME}
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${_asset_dir}"
            DEST_SUBDIR "${_asset_dir}"
        )
    endforeach()
endfunction()