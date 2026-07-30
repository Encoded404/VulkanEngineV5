module;

export module VulkanEngine.TechniqueManager.DefaultMeshTechnique;

import std;

import vulkan_hpp;

import VulkanEngine.TechniqueManager.BaseTechnique;
import VulkanEngine.GpuResources.BlockArray;
import VulkanEngine.ECS.ComponentRegistry;

export namespace VulkanEngine::TechniqueManager {

struct DefaultMeshPerMaterialData {
    std::uint32_t albedo_texture{0};
    std::uint32_t normal_texture{0};
    std::uint32_t orm_texture{0};       // occlusion(R) roughness(G) metallic(B)
    float     roughness_factor{1.0f};
    float     metallic_factor{0.0f};
    float     ao_factor{1.0f};
};

class DefaultMeshTechnique final : public BaseTechnique {
public:
    DefaultMeshTechnique() {
        // Set 5 = first technique custom set (engine sets 0-4 are reserved).
        // Binding 0 = first (only) PerMaterial binding for this technique.
        DeclarePerMaterial<DefaultMeshPerMaterialData>(5, 0);
    }

    // ── MaterialHandle<Tech> requires these static helpers ──

    template<typename T>
    static constexpr bool HasBinding() {
        return std::is_same_v<T, DefaultMeshPerMaterialData>;
    }

    template<typename T>
    static constexpr std::size_t GetOffset() {
        static_assert(std::is_same_v<T, DefaultMeshPerMaterialData>,
                      "DefaultMeshTechnique only has DefaultMeshPerMaterialData");
        return 0;  // only one PerMaterial type, at offset 0 in cpu_data
    }

    template<typename T>
    static constexpr std::uint32_t GetBindingIndex() {
        static_assert(std::is_same_v<T, DefaultMeshPerMaterialData>,
                      "DefaultMeshTechnique only has DefaultMeshPerMaterialData");
        return 0;  // first (only) PerMaterial binding
    }

    // Compile the default mesh pipeline with the given SPIR-V and engine layouts
    void CompileDefaultMesh(VulkanBackend::Vulkan::VulkanBootstrap& bootstrap,
                            std::span<const std::uint32_t> vert_spv,
                            std::span<const std::uint32_t> frag_spv,
                            const VulkanEngine::StandardMeshPipeline::PipelineConfig& config,
                            vk::DescriptorSetLayout bindless_layout,
                            vk::DescriptorSetLayout submesh_vertex_layout,
                            vk::DescriptorSetLayout raw_vertex_layout,
                            vk::DescriptorSetLayout indirection_layout,
                            vk::DescriptorSetLayout scene_uniform_layout = nullptr) {
        Compile(bootstrap, vert_spv, frag_spv, config,
                bindless_layout, submesh_vertex_layout, raw_vertex_layout, indirection_layout,
                scene_uniform_layout);
    }

    [[nodiscard]] std::uint32_t PackMaterialData(std::uint32_t material_id) const override {
        constexpr std::uint32_t kTechBits = 12;
        constexpr std::uint32_t kTechMask = (1u << kTechBits) - 1;
        return (material_id << kTechBits) | (GetId().value & kTechMask);
    }

    [[nodiscard]] VulkanEngine::GpuResources::BlockArray* GetMaterialBlockArray() {
        return GetBlockArrayForType<DefaultMeshPerMaterialData>();
    }
};

} // namespace VulkanEngine::TechniqueManager
