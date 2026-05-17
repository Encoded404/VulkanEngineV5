add_executable(${APP_TARGET})
target_sources(${APP_TARGET} PRIVATE
    main.cpp
    demoscene/DemoSceneRenderer.cpp
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_sources(${APP_TARGET} PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            Components/DemoInputComponent.cppm
            demoscene/DemoSceneRenderer.cppm
    )
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

include(ShaderUtils)
compile_shaders(${APP_TARGET}_shaders)
target_link_shaders(${APP_TARGET} ${APP_TARGET}_shaders)

include(CopyAppAssets)
configure_app_asset_copy(${APP_TARGET}
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/models"
    DEST_SUBDIR "models"
)
configure_app_asset_copy(${APP_TARGET}
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/textures"
    DEST_SUBDIR "textures"
)
