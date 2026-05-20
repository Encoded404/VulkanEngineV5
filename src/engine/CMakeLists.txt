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
find_package(Stb REQUIRED)
find_package(imgui CONFIG REQUIRED)

target_link_libraries(${ENGINE_TARGET} PRIVATE
    Vulkan::Vulkan
    KTX::ktx
    glm::glm
    fastgltf::fastgltf
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
    Input/Input.cpp
    FileLoaders/Textures/TextureLoaders.cpp
    FileLoaders/Textures/TextureLoaders.cppm
    ResourceSystem/ResourceSystem.cpp
    ResourceSystem/TextureResource.cpp
    ImGui/ImGuiSystem.cpp
    RenderPipeline/RenderPipeline.cpp
    GpuResources/GpuBuffer.cpp
    GpuResources/GpuTexture.cpp
    GpuResources/GpuDescriptorSet.cpp
    ShaderLoader/ShaderLoader.cpp
    DefaultResources/DefaultResources.cpp
    MeshRendererSystem/MeshRendererSystem.cpp
    StandardMeshPipeline/StandardMeshPipeline.cpp
    SceneLoader/SceneLoader.cpp
    DefaultRenderer/DefaultRenderer.cpp
    TechniqueManager/TechniqueManager.cpp
    MaterialManager/MaterialManager.cpp
    SceneRenderer/SceneRenderer.cpp
    BindlessManager/BindlessManager.cpp
)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_sources(${ENGINE_TARGET} PUBLIC
            FILE_SET CXX_MODULES
            BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
            FILES
                Input/Input.cppm
                Mesh/MeshTypes.cppm
                ResourceSystem/ResourceSystem.cppm
                ResourceSystem/TextureResource.cppm
                Application/Application.cppm
                Startup/Startup.cppm
                Components/Transform.cppm
                Components/MeshRenderer.cppm
                Components/Camera.cppm
                Components/Material.cppm
                Components/MeshReference.cppm
                FileLoaders/Mesh/BinMeshAssembler.cppm
                FileLoaders/Mesh/GltfMeshAssembler.cppm
                FileLoaders/Textures/TextureLoaders.cppm
                ImGui/ImGuiSystem.cppm
                RenderPipeline/RenderPipeline.cppm
                GpuResources/GpuResources.cppm
                GpuResources/GpuBuffer.cppm
                GpuResources/GpuTexture.cppm
                GpuResources/GpuDescriptorSet.cppm
                ShaderLoader/ShaderLoader.cppm
                DefaultResources/DefaultResources.cppm
                MeshRendererSystem/MeshRendererSystem.cppm
                StandardMeshPipeline/StandardMeshPipeline.cppm
                SceneLoader/SceneLoader.cppm
                DefaultRenderer/DefaultRenderer.cppm
                TechniqueManager/TechniqueManager.cppm
                MaterialManager/MaterialManager.cppm
                SceneRenderer/SceneRenderer.cppm
                BindlessManager/BindlessManager.cppm
        )
    endif()

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(${ENGINE_TARGET})
endif()

include(ShaderUtils)
compile_shaders(${ENGINE_TARGET}_shaders)
target_link_shaders(${ENGINE_TARGET} ${ENGINE_TARGET}_shaders)

# Expose SHADER_DIR to dependent targets (e.g. the app)
get_target_property(_engine_shader_dir ${ENGINE_TARGET}_shaders SHADER_OUTPUT_DIR)
target_compile_definitions(${ENGINE_TARGET} PUBLIC
    SHADER_DIR="$<$<CONFIG:Debug>:${_engine_shader_dir}>$<$<CONFIG:Release>:${_engine_shader_dir}>$<$<CONFIG:RelWithDebInfo>:${_engine_shader_dir}>$<$<CONFIG:MinSizeRel>:${_engine_shader_dir}>"
)

