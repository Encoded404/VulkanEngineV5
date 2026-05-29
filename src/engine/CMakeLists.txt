add_library(${ENGINE_TARGET} STATIC)
target_compile_features(${ENGINE_TARGET} PUBLIC cxx_std_20)
target_link_libraries(${ENGINE_TARGET} PUBLIC logiface::logiface)

target_link_libraries(${ENGINE_TARGET} PRIVATE
    FileLoader
    ${BACKEND_TARGET}
)

find_package(Vulkan REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(fastgltf CONFIG REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(imgui CONFIG REQUIRED)

target_link_libraries(${ENGINE_TARGET} PRIVATE
    Vulkan::Vulkan
    KTX::ktx
    glm::glm
    fastgltf::fastgltf
    tinyobjloader::tinyobjloader
    imgui::imgui
)

target_include_directories(${ENGINE_TARGET} PUBLIC
    ${Stb_INCLUDE_DIR}
)

target_include_directories(${ENGINE_TARGET} PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_options(${ENGINE_TARGET} PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:Clang>:-Wall -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

if(CMAKE_CONFIGURATION_TYPES)
    target_compile_options(${ENGINE_TARGET} PRIVATE
        "$<$<CONFIG:Debug>:-g>"
        "$<$<CONFIG:Debug>:-O0>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

target_precompile_headers(${ENGINE_TARGET} PRIVATE
    ${CMAKE_SOURCE_DIR}/cmake/pch_vulkan.hpp
    ${CMAKE_SOURCE_DIR}/cmake/pch_core.hpp
)

target_sources(${ENGINE_TARGET} PRIVATE
    platform/Input.cpp
    gpu/GpuResources/BlockBuffer.cpp
    assets/FileLoaders/Mesh/MeshLoaderBase.cpp
    assets/FileLoaders/Textures/TextureLoaders.cpp
    assets/FileLoaders/Textures/TextureLoaders.cppm
    assets/ResourceSystem/ResourceSystem.cpp
    assets/ResourceSystem/TextureResource.cpp
    platform/ImGuiSystem.cpp
    rendering/RenderPipeline.cpp
    gpu/GpuResources/GpuBuffer.cpp
    gpu/GpuResources/GpuTexture.cpp
    gpu/GpuResources/GpuDescriptorSet.cpp
    gpu/GpuResources/StagingManager.cpp
    gpu/GpuResources/DeviceBufferHeap.cpp
    assets/ShaderLoader.cpp
    rendering/DefaultResources.cpp
    rendering/MeshRendererSystem.cpp
    rendering/StandardMeshPipeline.cpp
    assets/SceneLoader.cpp
    rendering/Renderer.cpp
    rendering/TechniqueManager/TechniqueManager.cpp
    rendering/MaterialManager/MaterialManager.cpp
    rendering/SceneRenderer.cpp
    rendering/SceneRendererPipeline.cpp
    rendering/SceneRendererFrame.cpp
    rendering/BindlessManager/BindlessManager.cpp
    core/Game.cpp
)

include(ShaderUtils)
compile_shaders(${ENGINE_TARGET}_shaders SUBDIR "engine")
target_link_shaders(${ENGINE_TARGET} ${ENGINE_TARGET}_shaders)

# Expose SHADER_DIR to dependent targets (e.g. the app)
get_target_property(_engine_shader_dir ${ENGINE_TARGET}_shaders SHADER_OUTPUT_DIR)
target_compile_definitions(${ENGINE_TARGET} PUBLIC
    SHADER_DIR="$<$<CONFIG:Debug>:${_engine_shader_dir}>$<$<CONFIG:Release>:${_engine_shader_dir}>$<$<CONFIG:RelWithDebInfo>:${_engine_shader_dir}>$<$<CONFIG:MinSizeRel>:${_engine_shader_dir}>"
)

# Generate C++ module files with #embed for each compiled shader
include(ShaderModuleGen)
set(_shader_gen_dir "${CMAKE_BINARY_DIR}/shaders/engine")
generate_shader_modules(${ENGINE_TARGET}_shader_modules
    NAMESPACE "Engine"
    OUTPUT_DIR ${_shader_gen_dir}
)
add_dependencies(${ENGINE_TARGET} ${ENGINE_TARGET}_shader_modules)
get_target_property(_gen_shader_mods ${ENGINE_TARGET}_shader_modules GENERATED_SHADER_MODULES)

# #embed is a Clang extension in C++20 mode; suppress the warning for generated shader modules
if(_gen_shader_mods)
    set_source_files_properties(${_gen_shader_mods} PROPERTIES
        COMPILE_OPTIONS "-Wno-c23-extensions"
    )
endif()

    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_sources(${ENGINE_TARGET} PUBLIC
            FILE_SET CXX_MODULES
            BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
            FILES
                platform/Input.cppm
                assets/MeshTypes.cppm
                assets/ResourceSystem/ResourceSystem.cppm
                assets/ResourceSystem/TextureResource.cppm
                core/Application.cppm
                core/Startup.cppm
                platform/Components/Transform.cppm
                platform/Components/MeshRenderer.cppm
                platform/Components/Camera.cppm
                platform/Components/MeshReference.cppm
                assets/FileLoaders/Mesh/BinMeshAssembler.cppm
                assets/FileLoaders/Mesh/GltfMeshAssembler.cppm
                assets/FileLoaders/Mesh/MeshLoaderBase.cppm
                assets/FileLoaders/Mesh/ObjMeshAssembler.cppm
                assets/FileLoaders/Mesh/MeshMagicLoader.cppm
                assets/FileLoaders/Textures/TextureLoaders.cppm
                platform/ImGuiSystem.cppm
                rendering/RenderPipeline.cppm
                gpu/GpuResources/GpuResources.cppm
                gpu/GpuResources/GpuBuffer.cppm
                gpu/GpuResources/BlockBuffer.cppm
                gpu/GpuResources/GpuTexture.cppm
                gpu/GpuResources/GpuDescriptorSet.cppm
                gpu/GpuResources/StagingManager.cppm
                gpu/GpuResources/DeviceBufferHeap.cppm
                assets/ShaderLoader.cppm
                rendering/DefaultResources.cppm
                rendering/MeshRendererSystem.cppm
                rendering/StandardMeshPipeline.cppm
                assets/SceneLoader.cppm
                rendering/Renderer.cppm
                rendering/TechniqueManager/TechniqueManager.cppm
                rendering/TechniqueManager/TechniqueId.cppm
                rendering/MaterialManager/MaterialManager.cppm
                rendering/MaterialManager/MaterialId.cppm
                rendering/SceneRenderer.cppm
                rendering/BindlessManager/BindlessManager.cppm
                rendering/BindlessManager/TextureSlot.cppm
                core/Game.cppm
        )
    # Register generated shader module files
    if(_gen_shader_mods)
        target_sources(${ENGINE_TARGET} PUBLIC
            FILE_SET CXX_MODULES
            BASE_DIRS ${_shader_gen_dir}
            FILES ${_gen_shader_mods}
        )
    endif()
endif()

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(${ENGINE_TARGET})
endif()

