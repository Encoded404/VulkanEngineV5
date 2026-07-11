module;

#include <logging/logging_macros.hpp>

module VulkanEngine.EngineBootstrap;

import std;
import std.compat;
import logiface;
import vulkan_hpp;

import VulkanEngine.BindlessManager;
import VulkanEngine.GpuResources;
import VulkanEngine.DefaultTextureFactory;
import VulkanEngine.MeshManager;
import VulkanEngine.MeshRegistry;
import VulkanEngine.MeshRenderSystem;
import VulkanEngine.MaterialManager;
import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;
import VulkanEngine.EngineContext;

namespace VulkanEngine {

bool EngineBootstrap::Initialize(EngineContext& ctx,
                                  const GameConfig& config,
                                  VulkanBackend::Vulkan::VulkanBootstrap& backend) {
    auto& vk_backend = backend.GetBackend();

    ctx.missing_texture = DefaultTextureFactory::CreateCheckerboard(ctx.resource_manager);
    ctx.fallback_handle = ResourceHandle<TextureResource>(ResourceId{"checkerboard_default"}, &ctx.resource_manager);

    ctx.bindless_mgr = std::make_unique<BindlessManager::BindlessManager>();
    if (!ctx.bindless_mgr->Initialize(vk_backend)) {
        return false;
    }

    GpuResources::HeapConfig heap_config{};
    heap_config.block_size = config.geometry_buffer_size_mb << 20;
    if (!ctx.vertex_heap.Initialize(vk_backend, heap_config, "vertex")) return false;
    if (!ctx.index_heap.Initialize(vk_backend, heap_config, "index")) return false;
    if (!ctx.staging_mgr.Initialize(vk_backend)) return false;

    {
        GpuResources::HeapConfig dynamic_heap_config{};
        dynamic_heap_config.block_size = 32ULL << 20;
        dynamic_heap_config.memory_flags =
            vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent;

        for (std::uint32_t i = 0; i < FRAMES_IN_FLIGHT_DYN; ++i) {
            if (!ctx.dynamic_vertex_heaps[i].Initialize(vk_backend, dynamic_heap_config,
                "dynamic_vertex_fifo" + std::to_string(i))) return false;
            if (!ctx.dynamic_index_heaps[i].Initialize(vk_backend, dynamic_heap_config,
                "dynamic_index_fifo" + std::to_string(i))) return false;
        }
    }

    ctx.mesh_manager = std::make_unique<MeshManager>();
    if (!ctx.mesh_manager->Initialize(vk_backend, &ctx.vertex_heap, &ctx.index_heap,
                                       &ctx.staging_mgr,
                                       ctx.dynamic_vertex_heaps.data(),
                                       ctx.dynamic_index_heaps.data(),
                                       FRAMES_IN_FLIGHT_DYN)) {
        return false;
    }

    return true;
}

void EngineBootstrap::Shutdown(EngineContext& ctx,
                                VulkanBackend::Vulkan::VulkanBootstrap& backend) {
    try {
        backend.GetBackend().GetDevice().waitIdle();
    } catch (...) {
        LOGIFACE_LOG(warn, "Exception during GPU wait idle in EngineBootstrap shutdown");
    }

    if (ctx.renderer) {
        ctx.renderer->Shutdown();
        ctx.renderer.reset();
    }
    if (ctx.imgui_system) {
        ctx.imgui_system->Shutdown();
        ctx.imgui_system.reset();
    }
    ctx.imgui_backend.reset();
    if (ctx.scene_renderer) {
        ctx.scene_renderer->Shutdown();
        ctx.scene_renderer.reset();
    }
    ctx.mesh_registry.Shutdown();
    if (ctx.mesh_manager) {
        ctx.mesh_manager->Shutdown();
        ctx.mesh_manager.reset();
    }
    for (auto& heap : ctx.dynamic_vertex_heaps) heap.Shutdown();
    for (auto& heap : ctx.dynamic_index_heaps) heap.Shutdown();
    ctx.staging_mgr.Shutdown();
    ctx.vertex_heap.Shutdown();
    ctx.index_heap.Shutdown();
    if (ctx.bindless_mgr) {
        ctx.bindless_mgr->Shutdown();
        ctx.bindless_mgr.reset();
    }
    ctx.material_mgr.Shutdown();
    if (ctx.technique_mgr) {
        ctx.technique_mgr->Shutdown();
        ctx.technique_mgr.reset();
    }
}

} // namespace VulkanEngine
