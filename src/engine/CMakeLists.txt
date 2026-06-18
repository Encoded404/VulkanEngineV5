add_library(${ENGINE_TARGET} STATIC)
target_compile_features(${ENGINE_TARGET} PUBLIC cxx_std_23)
target_link_libraries(${ENGINE_TARGET} PUBLIC logiface::logiface)

target_link_libraries(${ENGINE_TARGET} PRIVATE
    FileLoader
    ${BACKEND_TARGET}
)

find_package(Vulkan REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)

# fastgltf as a git submodule, built with C++20 modules + import std;
# We copy the source to the build directory and patch it there so the
# submodule tree is never modified.
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
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:RelWithDebInfo>:-O2>"
        "$<$<CONFIG:Release>:-O3>"
    )
endif()

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
    rendering/PipelinePass.cpp
    rendering/TechniqueManager/BaseTechnique.cpp
    gpu/GpuResources/GpuBuffer.cpp
    gpu/GpuResources/GpuTexture.cpp
    gpu/GpuResources/GpuDescriptorSet.cpp
    gpu/GpuResources/StagingManager.cpp
    gpu/GpuResources/DeviceBufferHeap.cpp
    gpu/GpuResources/TlsfAllocator.cpp
    assets/ShaderLoader.cpp
    rendering/DefaultResources.cpp
    rendering/MeshManager/MeshManager.cpp
    rendering/MeshRendererSystem.cpp
    rendering/MeshRenderSystem.cpp
    rendering/StandardMeshPipeline.cpp
    assets/SceneLoader.cpp
    assets/MeshRegistry.cpp
    platform/Components/DynamicMesh.cpp
    rendering/Renderer.cpp
    rendering/TechniqueManager/TechniqueManager.cpp
    rendering/MaterialManager/MaterialManager.cpp
    rendering/SceneRenderer.cpp
    rendering/SceneRendererPipeline.cpp
    rendering/SceneRendererFrame.cpp
    rendering/BindlessManager/BindlessManager.cpp
    core/Game.cpp
)

# Compile slang shaders to SPIR-V + generate C++20 modules with #embed
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/external/SlangSpriVCompilerHelper/cmake")
include(SlangSpirVCompiler)
set(_shader_gen_dir "${CMAKE_BINARY_DIR}/generated/shaders/engine")
set(_shader_opt_level
    "$<$<CONFIG:Debug>:0>$<$<CONFIG:RelWithDebInfo>:2>$<$<CONFIG:Release>:3>")
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
        CollectWriteDgcComp     collect_write_dgc.slang    compute  main
        CollectWriteLegacyComp  collect_write_legacy.slang compute  main
        MainIndirVert           main_indir.slang           vertex   main
        DepthIndirVert          depth_indir.slang          vertex   main
        StandardMeshFrag        standard_mesh.slang        fragment main
        DepthPrepassFrag        depth_prepass.slang        fragment main
        DegenerateVert          degenerate.slang           vertex   vert_main
        DegenerateFrag          degenerate.slang           fragment frag_main
)
add_dependencies(${ENGINE_TARGET} ${ENGINE_TARGET}_shaders)
get_target_property(_gen_shader_mods ${ENGINE_TARGET}_shaders SLANG_CPPM_FILES)

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
            platform/Components/DynamicMesh.cppm
            assets/FileLoaders/Mesh/BinMeshAssembler.cppm
            assets/FileLoaders/Mesh/GltfMeshAssembler.cppm
            assets/FileLoaders/Mesh/MeshLoaderBase.cppm
            assets/FileLoaders/Mesh/ObjMeshAssembler.cppm
            assets/FileLoaders/Mesh/MeshMagicLoader.cppm
            assets/FileLoaders/Textures/TextureLoaders.cppm
            platform/ImGuiSystem.cppm
            rendering/RenderPipeline.cppm
            rendering/PipelinePass.cppm
        gpu/GpuResources/GpuResources.cppm
        gpu/GpuResources/GpuBuffer.cppm
        gpu/GpuResources/BlockBuffer.cppm
        gpu/GpuResources/GpuTexture.cppm
        gpu/GpuResources/GpuDescriptorSet.cppm
        gpu/GpuResources/StagingManager.cppm
        gpu/GpuResources/DeviceBufferHeap.cppm
        gpu/GpuResources/TlsfAllocator.cppm
        gpu/GpuResources/MeshData.cppm
        assets/ShaderLoader.cppm
        rendering/DefaultResources.cppm
        rendering/MeshManager/MeshManager.cppm
            rendering/MeshRendererSystem.cppm
            rendering/MeshRenderSystem.cppm
            rendering/StandardMeshPipeline.cppm
            assets/SceneLoader.cppm
            assets/MeshRegistry.cppm
            rendering/Renderer.cppm
            rendering/TechniqueManager/TechniqueManager.cppm
            rendering/TechniqueManager/BaseTechnique.cppm
            rendering/TechniqueManager/TechniqueId.cppm
            rendering/MaterialManager/MaterialManager.cppm
            rendering/MaterialManager/MaterialId.cppm
            rendering/SceneRenderer.cppm
            rendering/BindlessManager/BindlessManager.cppm
            rendering/BindlessManager/TextureSlot.cppm
            core/Game.cppm
    )
    # Generated slang shader modules
    if(_gen_shader_mods)
        target_sources(${ENGINE_TARGET} PUBLIC
            FILE_SET CXX_MODULES
            BASE_DIRS ${_shader_gen_dir}
            FILES ${_gen_shader_mods}
        )
    endif()
endif()

# Link shared ShaderReflection module (OBJECT library from SlangSpriVCompilerHelper)
target_link_libraries(${ENGINE_TARGET} PUBLIC slang_shared_reflection)

if(COMMAND enable_target_clang_tidy)
    enable_target_clang_tidy(${ENGINE_TARGET})
endif()

