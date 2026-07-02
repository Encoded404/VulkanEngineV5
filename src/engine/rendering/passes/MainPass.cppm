module;

export module VulkanEngine.Render.Passes.MainPass;

import std;
import vulkan_hpp;
import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanBackend.Component;
import VulkanEngine.TechniqueManager;
import VulkanEngine.BindlessManager;

export namespace VulkanEngine::SceneRenderer {

class MainPass {
public:
    MainPass() = default;
    ~MainPass() = default;

    void Execute(vk::CommandBuffer cmd,
                 VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr,
                 VulkanEngine::BindlessManager::BindlessManager& bindless_mgr,
                 vk::DescriptorSet submesh_vertex_set,
                 vk::DescriptorSet bindless_vertex_set,
                 vk::DescriptorSet indirection_raw_set,
                 vk::Buffer technique_draw_commands_buffer,
                 std::uint32_t width, std::uint32_t height,
                 std::uint32_t entity_count);
};

} // namespace VulkanEngine::SceneRenderer
