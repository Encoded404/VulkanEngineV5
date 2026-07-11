add_library(${ENGINE_TARGET} STATIC)
target_compile_features(${ENGINE_TARGET} PUBLIC cxx_std_23)
target_link_libraries(${ENGINE_TARGET} PUBLIC logiface::logiface)

target_link_libraries(${ENGINE_TARGET} PRIVATE
    FileLoader
    ${BACKEND_TARGET}
    VulkanShared
)

find_package(Vulkan REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)

include(PatchFastgltfSubmodule)
patch_fastgltf_submodule(
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/fastgltf"
    PATCHED_DIR_VAR _patched_fastgltf_dir
)
set(FASTGLTF_COMPILE_AS_CPP20 ON CACHE BOOL "")
set(FASTGLTF_ENABLE_CPP_MODULES ON CACHE BOOL "")
set(FASTGLTF_USE_STD_MODULE ON CACHE BOOL "")
add_subdirectory(${_patched_fastgltf_dir} ${CMAKE_CURRENT_BINARY_DIR}/fastgltf)
find_package(Stb REQUIRED)
find_package(imgui CONFIG REQUIRED)

target_link_libraries(${ENGINE_TARGET} PRIVATE
    Vulkan::HppModule
    KTX::ktx
    glm::glm
    fastgltf::module
    tinyobjloader::tinyobjloader
    imgui::imgui
)

target_include_directories(${ENGINE_TARGET} PUBLIC ${Stb_INCLUDE_DIR})
target_include_directories(${ENGINE_TARGET} PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(${ENGINE_TARGET} PUBLIC
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
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
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

target_sources(${ENGINE_TARGET} PRIVATE
    input/InputSystem.cpp
    gpu/BlockArray.cpp
    assets/FileLoaders/Mesh/MeshLoaderBase.cpp
    assets/FileLoaders/Textures/TextureLoaders.cpp
    assets/FileLoaders/Textures/TextureLoaders.cppm
    assets/ResourceCache.cpp
    assets/TextureResource.cpp
    ui/ImGuiSystem.cpp
    render/Pipeline.cpp
    render/PipelinePass.cpp
    render/techniques/BaseTechnique.cpp
    gpu/Buffer.cpp
    gpu/Texture.cpp
    gpu/DescriptorSet.cpp
    gpu/StagingRing.cpp
    gpu/BufferHeap.cpp
    gpu/TlsfAllocator.cpp
    assets/ShaderLoader.cpp
    assets/DefaultTextures.cpp
    render/MeshUploadManager.cpp
    render/MeshGatherSystem.cpp
    render/MeshPipeline.cpp
    assets/SceneImport.cpp
    assets/MeshCache.cpp
    components/DynamicMesh.cpp
    render/Renderer.cpp
    render/techniques/TechniqueManager.cpp
    render/materials/MaterialManager.cpp
    render/SceneRenderer.cpp
    render/SceneRendererPipeline.cpp
    render/SceneRendererFrame.cpp
    render/bindless/BindlessManager.cpp
    core/GameEngine.cpp
    core/EngineBootstrap.cpp
    render/passes/ExpandPass.cpp
    render/passes/HiZPass.cpp
    render/passes/OcclusionPass.cpp
    render/passes/CollectPass.cpp
    render/passes/DepthPrePass.cpp
    render/passes/MainPass.cpp
    render/passes/ExpandPass.cppm
    render/passes/HiZPass.cppm
    render/passes/OcclusionPass.cppm
    render/passes/CollectPass.cppm
    render/passes/DepthPrePass.cppm
    render/passes/MainPass.cppm
    render_graph/RenderGraph.cpp
    render_graph/RenderGraphCompile.cpp
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/external/SlangSpriVCompilerHelper/cmake")
include(SlangSpirVCompiler)
set(_shader_gen_dir "${CMAKE_BINARY_DIR}/generated/shaders/engine")
set(_shader_opt_level "$<$<CONFIG:Debug>:0>$<$<CONFIG:RelWithDebInfo>:2>$<$<CONFIG:Release>:3>")
add_slang_shaders(
    TARGET      ${ENGINE_TARGET}_shaders
    OUTPUT_DIR  ${_shader_gen_dir}
    NAMESPACE   Engine
    SHADER_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    COMPILER    slang-spirv-compiler
    OPT_LEVEL   ${_shader_opt_level}
    SHADERS
        ExpandComp              expand.slang              compute  main
        OcclusionCullComp       occlusion_cull.slang       compute  main
        HizGenComp              hiz_gen.slang              compute  main
        CollectCountCompactComp collect_count_compact.slang compute  main
        CollectWriteComp        collect_write.slang        compute  main
        MainIndirVert           main_indir.slang           vertex   main
        DepthIndirVert          depth_indir.slang          vertex   main
        StandardMeshFrag        standard_mesh.slang        fragment main
        DepthPrepassFrag        depth_prepass.slang        fragment main
)
add_dependencies(${ENGINE_TARGET} ${ENGINE_TARGET}_shaders)
get_target_property(_gen_shader_mods ${ENGINE_TARGET}_shaders SLANG_CPPM_FILES)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_sources(${ENGINE_TARGET} PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
        FILES
            input/InputSystem.cppm
            assets/MeshTypes.cppm
            assets/ResourceCache.cppm
            assets/TextureResource.cppm
            core/Startup.cppm
            core/ApplicationContext.cppm
            ecs/ComponentRegistry.cppm
            components/Transform.cppm
            components/Camera.cppm
            components/MeshReference.cppm
            components/DynamicMesh.cppm
            assets/FileLoaders/Mesh/BinMeshAssembler.cppm
            assets/FileLoaders/Mesh/GltfMeshAssembler.cppm
            assets/FileLoaders/Mesh/MeshLoaderBase.cppm
            assets/FileLoaders/Mesh/ObjMeshAssembler.cppm
            assets/FileLoaders/Mesh/MeshMagicLoader.cppm
            assets/FileLoaders/Textures/TextureLoaders.cppm
            ui/ImGuiSystem.cppm
            render/Pipeline.cppm
            render/PipelinePass.cppm
            gpu/GpuResources.cppm
            gpu/Buffer.cppm
            gpu/BlockArray.cppm
            gpu/Texture.cppm
            gpu/DescriptorSet.cppm
            gpu/StagingRing.cppm
            gpu/BufferHeap.cppm
            gpu/TlsfAllocator.cppm
            gpu/MeshData.cppm
            assets/ShaderLoader.cppm
            assets/DefaultTextures.cppm
            render/MeshUploadManager.cppm
            render/MeshGatherSystem.cppm
            render/MeshPipeline.cppm
            assets/SceneImport.cppm
            assets/MeshCache.cppm
            render/Renderer.cppm
            render/techniques/TechniqueManager.cppm
            render/techniques/BaseTechnique.cppm
            render/techniques/DefaultMeshTechnique.cppm
            render/techniques/TechniqueId.cppm
            render/materials/MaterialManager.cppm
            render/materials/MaterialId.cppm
            render/materials/MaterialHandle.cppm
            render/SceneRenderer.cppm
            render/bindless/BindlessManager.cppm
            render/bindless/TextureSlot.cppm
            core/GameEngine.cppm
            core/EngineContext.cppm
            core/EngineBootstrap.cppm
            render_graph/RenderGraph.cppm
            render/passes/ExpandPass.cppm
            render/passes/HiZPass.cppm
            render/passes/OcclusionPass.cppm
            render/passes/CollectPass.cppm
            render/passes/DepthPrePass.cppm
            render/passes/MainPass.cppm
    )
    if(_gen_shader_mods)
        target_sources(${ENGINE_TARGET} PUBLIC
            FILE_SET CXX_MODULES
            BASE_DIRS ${_shader_gen_dir}
            FILES ${_gen_shader_mods}
        )
    endif()
endif()

target_link_libraries(${ENGINE_TARGET} PUBLIC slang_shared_reflection)

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(${ENGINE_TARGET})
endif()
