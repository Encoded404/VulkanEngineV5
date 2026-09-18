module;

export module VulkanEngine.EngineBootstrap;

import VulkanEngine.EngineContext;
import VulkanBackend.Vulkan.VulkanBootstrap;

export namespace VulkanEngine {

class EngineBootstrap {
public:
    bool Initialize(EngineContext& ctx,
                    const GameConfig& config,
                    VulkanBackend::Vulkan::VulkanBootstrap& backend);
    void Shutdown(EngineContext& ctx,
                  VulkanBackend::Vulkan::VulkanBootstrap& backend);
};

} // namespace VulkanEngine
