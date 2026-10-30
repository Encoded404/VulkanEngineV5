module;

export module VulkanEngine.EngineBootstrap;

import VulkanEngine.EngineContext;
import VulkanBackend.Vulkan.VulkanBootstrap;

export namespace VulkanEngine {

class EngineBootstrap {
public:
    // Device/core resources only: heaps, bindless, uploaders, meshes, shaders.
    // Optional subsystems (text, physical camera) are separate modules so a
    // game can decline them.
    bool Initialize(EngineContext& ctx,
                    const GameConfig& config,
                    VulkanBackend::Vulkan::VulkanBootstrap& backend);

    // Text pipeline bring-up (system + GPU glyph-atlas layer, and the font
    // hot-reload watcher/reloader under VKENGINE_HOT_RELOAD). Always returns
    // true: a device whose glyph-atlas layer cannot come up still boots with a
    // CPU-only text system, which is the supported text-less fallback. Returns
    // false only for a programming error (missing device resources).
    bool InitializeText(EngineContext& ctx,
                        const GameConfig& config,
                        VulkanBackend::Vulkan::VulkanBootstrap& backend);

    // Physical-camera bring-up. The camera is optional, so a failed device
    // initialization logs, leaves the system null and returns true; false is
    // reserved for a programming error (missing device resources or shaders).
    bool InitializePhysicalCamera(EngineContext& ctx,
                                  const GameConfig& config,
                                  VulkanBackend::Vulkan::VulkanBootstrap& backend);

    void Shutdown(EngineContext& ctx,
                  VulkanBackend::Vulkan::VulkanBootstrap& backend);
};

} // namespace VulkanEngine
