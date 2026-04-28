# Executable that links the static library
add_executable(${APP_TARGET})
target_sources(${APP_TARGET} PRIVATE
    main.cpp
    demoscene/DemoScene.cpp
)

# C++ module interface units: use CMake 3.28 FILE_SET to declare module
# interface/source units to the target. CMake will build these as module
# translation units in the proper phase for compilers that support C++ modules.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_sources(${APP_TARGET} PUBLIC
        FILE_SET CXX_MODULES
        FILES
            demoscene/DemoScene.cppm
            fileloaders/mesh/BinMeshAssembler.cppm
            fileloaders/mesh/GltfMeshAssembler.cppm
            components/Transform.cppm
    )
endif()

target_link_libraries(${APP_TARGET} PRIVATE ${LIBRARY_TARGET})

# Ensure the app target can see FileLoader headers
target_link_libraries(${APP_TARGET} PRIVATE FileLoader)

find_package(SDL3 CONFIG REQUIRED)
find_package(fastgltf CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS stacktrace_basic)
find_package(CLI11 CONFIG REQUIRED)

target_link_libraries(${APP_TARGET} PRIVATE
    SDL3::SDL3
    fastgltf::fastgltf
    Boost::stacktrace_basic
    CLI11::CLI11
)

# Optionally set executable output name; keep simple default
set_target_properties(${APP_TARGET} PROPERTIES OUTPUT_NAME ${APP_TARGET})

if(OUTPUT_PATH)
    set_target_properties(${APP_TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_PATH})
endif()

# Set the C++ standard and enable compiler features
target_compile_features(${APP_TARGET} PRIVATE cxx_std_20)

# Compiler-specific warning options for the library target
target_compile_options(${APP_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
)

# Multi-configuration generator options
if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${APP_TARGET} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

# Include directories
target_include_directories(${APP_TARGET} PRIVATE
    # include any app-specific includes here
)

# Add compile definitions for logging toggle
# Define LOGIFACE_ENABLE_LOGGING based on ENABLE_LOGGING
target_compile_definitions(${APP_TARGET}
    PUBLIC
        $<$<BOOL:${ENABLE_LOGGING}>:LOGIFACE_ENABLE_LOGGING=1>
        $<$<NOT:$<BOOL:${ENABLE_LOGGING}>>:LOGIFACE_ENABLE_LOGGING=0>
        LOGIFACE_DEFAULT_TO_FUNCTION=1
)

# Optionally enable per-target clang-tidy if the helper function is available.
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
