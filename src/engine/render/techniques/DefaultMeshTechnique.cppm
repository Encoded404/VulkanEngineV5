module;

export module VulkanEngine.TechniqueManager.DefaultMeshTechnique;

import std;

import vulkan_hpp;

import VulkanEngine.TechniqueManager.BaseTechnique;
import VulkanEngine.GpuResources.BlockArray;
import VulkanEngine.ECS.ComponentRegistry;

export namespace VulkanEngine::TechniqueManager {

struct DefaultMeshPerMaterialData {
    std::uint32_t texture_slot{0};
    std::uint16_t technique_id{0};
};

class DefaultMeshTechnique final : public BaseTechnique {
public:
    DefaultMeshTechnique() {
        DeclarePerMaterial<DefaultMeshPerMaterialData>(0, 5);
    }

    // Compile the default mesh pipeline with the given SPIR-V and engine layouts
    void CompileDefaultMesh(VulkanBackend::Runtime::VulkanBootstrap& bootstrap,
                            std::span<const std::uint32_t> vert_spv,
                            std::span<const std::uint32_t> frag_spv,
                            const VulkanEngine::StandardMeshPipeline::PipelineConfig& config,
                            vk::DescriptorSetLayout bindless_layout,
                            vk::DescriptorSetLayout submesh_vertex_layout,
                            vk::DescriptorSetLayout raw_vertex_layout,
                            vk::DescriptorSetLayout indirection_layout) {
        Compile(bootstrap, vert_spv, frag_spv, config,
                bindless_layout, submesh_vertex_layout, raw_vertex_layout, indirection_layout);
    }

    [[nodiscard]] VulkanEngine::GpuResources::BlockArray* GetMaterialBlockArray() {
        return GetBlockArrayForType<DefaultMeshPerMaterialData>();
    }
};

} // namespace VulkanEngine::TechniqueManager
