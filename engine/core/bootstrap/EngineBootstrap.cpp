module;

#include <logging/logging_macros.hpp>

module VulkanEngine.EngineBootstrap;

import std;
import std.compat;
import logiface;
import vulkan_hpp;

import VulkanShared.ScopedSection;
import VulkanShared.Teardown;

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
import VulkanEngine.ShaderManager;
import VulkanEngine.PipelineFactory;
import VulkanEngine.ShaderWatcher;

#ifdef VKENGINE_PHYSICAL_CAMERA
import VulkanEngine.PhysicalCameraSystem;
#endif

namespace VulkanEngine {

namespace {

VulkanShared::ScopedSection DebugSection(const std::string& name) {
    return VulkanShared::ScopedSection{name, [](const std::string& section, double ms) {
        LOGIFACE_LOG(debug, section + ": " + std::to_string(ms) + " ms");
    }};
}

} // anonymous namespace

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

        const std::uint32_t fif = backend.GetBackend().GetFramesInFlight();
        ctx.dynamic_vertex_heaps.resize(fif);
        ctx.dynamic_index_heaps.resize(fif);
        for (std::uint32_t i = 0; i < fif; ++i) {
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
                                       static_cast<std::uint32_t>(ctx.dynamic_vertex_heaps.size()))) {
        return false;
    }

    ctx.shader_manager = std::make_unique<ShaderSystem::ShaderManager>(
        vk_backend.GetDevice(), vk_backend.GetCapabilities(),
        config.shader_cache_dir);
    ctx.pipeline_factory = std::make_unique<ShaderSystem::PipelineFactory>(
        vk_backend.GetDevice(), vk_backend.GetCapabilities(),
        ctx.shader_manager->GetPipelineCacheRAII(), config.gpl_policy, config.gpl_structure);

    if (!config.shader_data_dir.empty()) {
        ctx.shader_ids.RegisterAll(*ctx.shader_manager, config.shader_data_dir);
    }

#ifdef VKENGINE_PHYSICAL_CAMERA
    if (config.enable_physical_camera) {
        ctx.physical_camera = std::make_unique<PhysicalCamera::PhysicalCameraSystem>();
        if (!ctx.physical_camera->Initialize(vk_backend, *ctx.bindless_mgr,
                                              *ctx.shader_manager, *ctx.pipeline_factory,
                                              ctx.shader_ids.physical_camera_composite_vert,
                                              ctx.shader_ids.physical_camera_composite_frag)) {
            ctx.physical_camera.reset();
        }
    }
#endif

    return true;
}

void EngineBootstrap::Shutdown(EngineContext& ctx,
                                VulkanBackend::Vulkan::VulkanBootstrap& backend) {
    {
        auto s = DebugSection("engineshutdown.wait_idle");
        try {
            backend.GetBackend().GetDevice().waitIdle();
        } catch (...) {
            LOGIFACE_LOG(warn, "Exception during GPU wait idle in EngineBootstrap shutdown");
        }
    }

    // Everything below owns GPU resources (safe to destroy now that the device is
    // idle) and, except for the two edges noted below, destroys disjoint state,
    // so subsystems are torn down in parallel on a small worker pool.
    //
    // Dependency edges:
    //   - imgui_backend  -> imgui_system: both touch the same shared backend object.
    //   - shader_manager -> shader_watcher: the watcher's efsw thread may be inside
    //     OnFileChanged(); StopAsync() quiesces it before the manager is destroyed.
    const std::size_t worker_count =
        std::min<std::size_t>(4, std::max<std::size_t>(2, std::thread::hardware_concurrency()));
    VulkanShared::TeardownScheduler teardown{worker_count};

    std::optional<VulkanShared::TeardownId> shader_watcher_id;
    if (ctx.shader_watcher) {
        shader_watcher_id = teardown.Add("engineshutdown.shader_watcher", [&ctx] {
            auto s = DebugSection("engineshutdown.shader_watcher");
            ctx.shader_watcher->StopAsync();
            ctx.shader_watcher.reset();
        });
    }

    if (ctx.pipeline_factory) {
        teardown.Add("engineshutdown.pipeline_factory", [&ctx] {
            auto s = DebugSection("engineshutdown.pipeline_factory");
            ctx.pipeline_factory.reset();
        });
    }

    if (ctx.renderer) {
        teardown.Add("engineshutdown.renderer", [&ctx] {
            auto s = DebugSection("engineshutdown.renderer");
            ctx.renderer->Shutdown();
            ctx.renderer.reset();
        });
    }

    std::optional<VulkanShared::TeardownId> imgui_system_id;
    if (ctx.imgui_system) {
        imgui_system_id = teardown.Add("engineshutdown.imgui_system", [&ctx] {
            auto s = DebugSection("engineshutdown.imgui_system");
            ctx.imgui_system->Shutdown();
            ctx.imgui_system.reset();
        });
    }
    if (ctx.imgui_backend) {
        std::vector<VulkanShared::TeardownId> deps;
        if (imgui_system_id) {
            deps.push_back(*imgui_system_id);
        }
        teardown.Add("engineshutdown.imgui_backend",
                     [&ctx] {
                         auto s = DebugSection("engineshutdown.imgui_backend");
                         ctx.imgui_backend.reset();
                     },
                     std::move(deps));
    }

    if (ctx.scene_renderer) {
        teardown.Add("engineshutdown.scene_renderer", [&ctx] {
            auto s = DebugSection("engineshutdown.scene_renderer");
            ctx.scene_renderer->Shutdown();
            ctx.scene_renderer.reset();
        });
    }

#ifdef VKENGINE_PHYSICAL_CAMERA
    if (ctx.physical_camera) {
        teardown.Add("engineshutdown.physical_camera", [&ctx] {
            auto s = DebugSection("engineshutdown.physical_camera");
            ctx.physical_camera->Shutdown();
            ctx.physical_camera.reset();
        });
    }
#endif

    if (ctx.shader_manager) {
        std::vector<VulkanShared::TeardownId> deps;
        if (shader_watcher_id) {
            deps.push_back(*shader_watcher_id);
        }
        teardown.Add("engineshutdown.shader_manager",
                     [&ctx] {
                         auto s = DebugSection("engineshutdown.shader_manager");
                         ctx.shader_manager.reset();
                     },
                     std::move(deps));
    }

    teardown.Add("engineshutdown.mesh_registry", [&ctx] {
        auto s = DebugSection("engineshutdown.mesh_registry");
        ctx.mesh_registry.Shutdown();
    });

    if (ctx.mesh_manager) {
        teardown.Add("engineshutdown.mesh_manager", [&ctx] {
            auto s = DebugSection("engineshutdown.mesh_manager");
            ctx.mesh_manager->Shutdown();
            ctx.mesh_manager.reset();
        });
    }

    teardown.Add("engineshutdown.dynamic_heaps", [&ctx] {
        auto s = DebugSection("engineshutdown.dynamic_heaps");
        for (auto& heap : ctx.dynamic_vertex_heaps) heap.Shutdown();
        for (auto& heap : ctx.dynamic_index_heaps) heap.Shutdown();
    });

    teardown.Add("engineshutdown.staging_manager", [&ctx] {
        auto s = DebugSection("engineshutdown.staging_manager");
        ctx.staging_mgr.Shutdown();
    });

    teardown.Add("engineshutdown.static_heaps", [&ctx] {
        auto s = DebugSection("engineshutdown.static_heaps");
        ctx.vertex_heap.Shutdown();
        ctx.index_heap.Shutdown();
    });

    if (ctx.bindless_mgr) {
        teardown.Add("engineshutdown.bindless_manager", [&ctx] {
            auto s = DebugSection("engineshutdown.bindless_manager");
            ctx.bindless_mgr->Shutdown();
            ctx.bindless_mgr.reset();
        });
    }

    teardown.Add("engineshutdown.material_manager", [&ctx] {
        auto s = DebugSection("engineshutdown.material_manager");
        ctx.material_mgr.Shutdown();
    });

    if (ctx.technique_mgr) {
        teardown.Add("engineshutdown.technique_manager", [&ctx] {
            auto s = DebugSection("engineshutdown.technique_manager");
            ctx.technique_mgr->Shutdown();
            ctx.technique_mgr.reset();
        });
    }

    try {
        teardown.Run();
    } catch (const std::exception& err) {
        LOGIFACE_LOG(warn, std::string("Exception during parallel engine shutdown: ") + err.what());
    }
}

} // namespace VulkanEngine
