module;

export module VulkanEngine.EngineBootstrap;

import VulkanEngine.EngineContext;
import VulkanBackend.Runtime.VulkanBootstrap;

export namespace VulkanEngine::Game {

class EngineBootstrap {
public:
    bool Initialize(EngineContext& ctx,
                    const GameConfig& config,
                    VulkanBackend::Runtime::VulkanBootstrap& backend);
    void Shutdown(EngineContext& ctx,
                  VulkanBackend::Runtime::VulkanBootstrap& backend);
};

} // namespace VulkanEngine::Game
