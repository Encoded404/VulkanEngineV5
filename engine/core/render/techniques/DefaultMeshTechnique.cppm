module;

export module VulkanEngine.TechniqueManager.DefaultMeshTechnique;

import std;

import vulkan_hpp;

import VulkanEngine.TechniqueManager.BaseTechnique;
import VulkanEngine.GpuResources.BlockArray;
import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.ShaderManager;
import VulkanEngine.PipelineFactory;

export namespace VulkanEngine::TechniqueManager {

// Final material contract: C layout, std430-safe, 64 bytes. The tail is
// float4-packed for unambiguous layout. flags bits 0..1 carry the declared
// TextureNormalEncoding (Standard/Full/Bent); bit 2 alpha_mask; bit 3
// has_transform. The encoding is declared content, never derived from the
// resolved upload format. `standard_mesh.slang` mirrors this struct exactly —
// change both together. No unlit bit: unlit is a separate technique.
struct DefaultMeshPerMaterialData {
    std::uint32_t albedo_texture{0};
    std::uint32_t normal_texture{0};
    std::uint32_t orm_texture{0};       // occlusion(R) roughness(G) metallic(B)
    std::uint32_t emissive_texture{0};
    std::uint32_t flags{0};             // bits0..1 normal encoding; bit2 alpha_mask; bit3 has_transform
    std::uint32_t uv_sets{0};           // 2 bits per texture slot (albedo/normal/orm/emissive)
    float     roughness_factor{1.0f};
    float     metallic_factor{0.0f};
    float     ao_factor{1.0f};
    float     normal_scale{1.0f};
    float     occlusion_strength{1.0f};
    float     alpha_cutoff{0.5f};
    float     emissive_factor[4]{0.0f, 0.0f, 0.0f, 1.0f}; // rgb + strength
};
static_assert(sizeof(DefaultMeshPerMaterialData) == 64);
static_assert(std::is_trivially_copyable_v<DefaultMeshPerMaterialData>);

constexpr std::uint32_t kNormalEncodingMask = 0x3u;
constexpr std::uint32_t kAlphaMaskFlag = 1u << 2;
constexpr std::uint32_t kHasTransformFlag = 1u << 3;

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

    // Compile the default mesh pipeline with the given SPIR-V and engine layouts.
    // Returns false if pipeline creation failed.
    [[nodiscard]] bool CompileDefaultMesh(VulkanBackend::Vulkan::VulkanBootstrap& bootstrap,
                            ShaderSystem::ShaderManager& shader_mgr,
                            ShaderSystem::PipelineFactory& pipeline_factory,
                            ShaderSystem::ShaderId vert_id,
                            ShaderSystem::ShaderId frag_id,
                            const VulkanEngine::StandardMeshPipeline::PipelineConfig& config,
                            vk::DescriptorSetLayout bindless_layout,
                            vk::DescriptorSetLayout submesh_vertex_layout,
                            vk::DescriptorSetLayout raw_vertex_layout,
                            vk::DescriptorSetLayout indirection_layout,
                            vk::DescriptorSetLayout scene_uniform_layout = nullptr) {
        return Compile(bootstrap, shader_mgr, pipeline_factory, vert_id, frag_id, config,
                       bindless_layout, submesh_vertex_layout, raw_vertex_layout, indirection_layout,
                       scene_uniform_layout);
    }

    // PackMaterialData uses the BaseTechnique default (TechniquePacking::Pack).

    [[nodiscard]] VulkanEngine::GpuResources::BlockArray* GetMaterialBlockArray() {
        return GetBlockArrayForType<DefaultMeshPerMaterialData>();
    }
};

} // namespace VulkanEngine::TechniqueManager
