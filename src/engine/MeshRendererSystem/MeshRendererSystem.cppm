module;

#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.MeshRendererSystem;

export import VulkanBackend.Component;
export import VulkanEngine.Components.Transform;
export import VulkanEngine.Components.MeshRenderer;
export import VulkanEngine.GpuResources;

export namespace VulkanEngine::MeshRendererSystem {

struct MeshRenderObject {
    const GpuResources::GpuBuffer* vertex_buffer = nullptr;
    const GpuResources::GpuBuffer* index_buffer = nullptr;
    vk::DescriptorSet descriptor_set{nullptr};
    const vk::Pipeline* pipeline = nullptr;
    const vk::PipelineLayout* pipeline_layout = nullptr;
    uint32_t index_count = 0;
};

class MeshRendererSystem {
public:
    void RecordAllMeshDraws(vk::CommandBuffer cmd,
                            VulkanEngine::ComponentRegistry& registry,
                            const MeshRenderObject& render_object,
                            uint32_t width,
                            uint32_t height);
};

} // namespace VulkanEngine::MeshRendererSystem
