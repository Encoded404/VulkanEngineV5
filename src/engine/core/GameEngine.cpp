module;

#include <logging/logging_macros.hpp>
#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)

module VulkanEngine.GameEngine;

import std;
import std.compat;
import logiface;

import vulkan_hpp;

import VulkanEngine.MeshManager;
import VulkanEngine.EngineBootstrap;
import VulkanEngine.ShaderWatcher;

namespace VulkanEngine {

GameEngine::~GameEngine() {
    if (initialized_) {
        Shutdown();
    }
}

bool GameEngine::Setup(VulkanEngine::Application::ApplicationContext& ctx, const GameConfig& config) {
    vk_backend_ = ctx.bootstrap;
    config_ = config;

    if (!bootstrap_.Initialize(ctx_, config, *ctx.bootstrap)) {
        return false;
    }

#ifdef VKENGINE_PHYSICAL_CAMERA
    if (ctx_.physical_camera) {
        physical_camera_sdl_token_ = ctx.platform->GetBackend().GetSdlEventProcessors().Register(
            [this](void* sdl_event) {
                if (ctx_.physical_camera) {
                    ctx_.physical_camera->ProcessSdlEvent(sdl_event);
                }
            });
    }
#endif

    initialized_ = true;
    return true;
}

uint32_t GameEngine::UploadTextureToBindless(VulkanEngine::Application::ApplicationContext& ctx, TextureResource* tex) {
    if (!tex || !tex->HasPixels()) return 0;
    auto gpu_tex = GpuResources::GpuTexture::CreateFromPixels(
        ctx.bootstrap->GetBackend(),
        reinterpret_cast<const uint8_t*>(tex->GetPixels().data()),
        tex->GetWidth(), tex->GetHeight());
    if (gpu_tex.IsValid()) {
        return ctx_.bindless_mgr->AllocateTextureSlot(std::move(gpu_tex), tex->GetId());
    }
    LOGIFACE_LOG(debug, "Failed to create GPU texture for: " + tex->GetId().value + ", using fallback");
    return 0;
}

uint32_t GameEngine::LoadTexture(VulkanEngine::Application::ApplicationContext& ctx, const std::filesystem::path& path) {
    auto tex_handle = SceneLoader::LoadTextureFromPath(
        ctx_.resource_manager, path, ctx_.fallback_handle);
    if (tex_handle.IsValid() && tex_handle->HasPixels()) {
        return UploadTextureToBindless(ctx, tex_handle.Get());
    }
    LOGIFACE_LOG(debug, "Failed to load texture from path: " + path.string() + ", using fallback");
    return 0;
}

bool GameEngine::InitRenderer(VulkanEngine::Application::ApplicationContext& ctx,
                              ShaderSystem::ShaderId vert_id,
                              ShaderSystem::ShaderId frag_id,
                              ShaderSystem::ShaderManager* shader_mgr) {
    auto& backend = ctx.bootstrap->GetBackend();

    constexpr std::uint32_t initial_indirection_entries = 1u << 20; // 1M entries = 8MB
    ctx_.scene_renderer = std::make_unique<SceneRenderer::SceneRenderer>();
    if (!ctx_.scene_renderer->Initialize(backend, ctx_.vertex_heap, initial_indirection_entries,
                                           ctx_.GetShaderManager(), ctx_.GetPipelineFactory(),
                                           ctx_.GetShaderIds(), backend.GetFramesInFlight())) {
        LOGIFACE_LOG(error, "SceneRenderer::Initialize failed");
        return false;
    }

    ctx_.technique_mgr = std::make_unique<TechniqueManager::TechniqueManager>();
    {
        auto mesh_tech = std::make_unique<TechniqueManager::DefaultMeshTechnique>();
        mesh_tech->CompileDefaultMesh(
            *ctx.bootstrap, *shader_mgr, *ctx_.pipeline_factory,
            vert_id, frag_id, config_.pipeline_config,
            *ctx_.bindless_mgr->GetLayout(),
            *ctx_.scene_renderer->GetSubmeshVertexDataLayout(),
            *ctx_.scene_renderer->GetRawVertexLayout(),
            *ctx_.scene_renderer->GetIndirectionLayout(),
            ctx_.scene_renderer->GetSceneUniformLayout());
        auto tech_id = ctx_.technique_mgr->Register(std::move(mesh_tech));
        main_technique_id_ = tech_id.value;
    }
    {
        // Unlit technique: same engine sets + PerMaterial layout as the main
        // technique, but paired with the unlit fragment shader (no lighting).
        auto unlit_tech = std::make_unique<TechniqueManager::UnlitTextureTechnique>();
        unlit_tech->CompileUnlit(
            *ctx.bootstrap, *shader_mgr, *ctx_.pipeline_factory,
            vert_id, ctx_.shader_ids.unlit_frag, config_.pipeline_config,
            *ctx_.bindless_mgr->GetLayout(),
            *ctx_.scene_renderer->GetSubmeshVertexDataLayout(),
            *ctx_.scene_renderer->GetRawVertexLayout(),
            *ctx_.scene_renderer->GetIndirectionLayout(),
            ctx_.scene_renderer->GetSceneUniformLayout());
        ctx_.technique_mgr->Register(std::move(unlit_tech));
    }

    ctx_.material_mgr.Initialize(&ctx_.staging_mgr);
    ctx_.material_mgr.SetTechniqueManager(ctx_.technique_mgr.get());

    // Upload initial lighting data via staging
    {
        SceneRenderer::SceneHeader header{};
        header.ambient_color[0] = 0.03f; header.ambient_color[1] = 0.03f;
        header.ambient_color[2] = 0.03f; header.ambient_color[3] = 1.0f;
        header.sun_direction[0] = 0.5f; header.sun_direction[1] = -0.707f;
        header.sun_direction[2] = 0.5f;
        header.sun_color[0] = 1.0f; header.sun_color[1] = 0.95f;
        header.sun_color[2] = 0.9f; header.sun_color[3] = 2.0f;

        SceneRenderer::Light sun_light{};
        sun_light.direction[0] = 0.5f; sun_light.direction[1] = -0.707f;
        sun_light.direction[2] = 0.5f;
        sun_light.color[0] = 1.0f; sun_light.color[1] = 0.95f;
        sun_light.color[2] = 0.9f; sun_light.color[3] = 2.0f;
        sun_light.position[3] = 0.0f; // type = directional

        std::array<SceneRenderer::Light, 1> lights = {sun_light};
        header.light_count = 1;
        ctx_.scene_renderer->UploadLighting(header, lights, ctx_.staging_mgr);
    }

    // Register fallback material (ID 0): main technique, bindless checkerboard
    const std::uint32_t fallback_slot = UploadTextureToBindless(ctx, ctx_.missing_texture.get());
    [[maybe_unused]] auto fallback_handle = ctx_.material_mgr.Register<TechniqueManager::DefaultMeshTechnique>(
        MaterialManager::BlendMode::Opaque,
        TechniqueManager::DefaultMeshPerMaterialData{
            .albedo_texture = fallback_slot,
            .roughness_factor = 1.0f,
            .metallic_factor = 0.0f,
            .ao_factor = 1.0f
        });

    ctx_.renderer = std::make_unique<Renderer::Renderer>();
    ctx_.renderer->Initialize(*ctx.bootstrap, config_.renderer_config, *ctx_.scene_renderer);

    if (config_.enable_imgui) {
        ctx_.imgui_backend = VulkanBackend::ImGui::CreateImGuiBackend();
        ctx_.imgui_system = std::make_unique<ImGui::ImGuiSystem>(ctx_.imgui_backend);

        const auto surface_format = backend.GetSurfaceFormat();
        VulkanBackend::ImGui::ImGuiBackendConfig imgui_backend_config{};
        imgui_backend_config.image_count = ctx.bootstrap->GetSnapshot().swapchain_image_count;
        imgui_backend_config.swapchain_format = static_cast<vk::Format>(surface_format.format);

        ImGui::ImGuiSystemInitInfo imgui_init_info{};
        imgui_init_info.sdl_window = ctx.window;
        imgui_init_info.backend_config = imgui_backend_config;
        imgui_init_info.instance = backend.GetInstance();
        imgui_init_info.physical_device = backend.GetPhysicalDevice();
        imgui_init_info.device = backend.GetDevice();
        imgui_init_info.queue_family = backend.GetGraphicsQueueFamily();
        imgui_init_info.queue = backend.GetGraphicsQueue();
        imgui_init_info.api_version = vk::ApiVersion13;

        [[maybe_unused]] const bool imgui_ok = ctx_.imgui_system->Initialize(imgui_init_info);

        auto& platform_backend = ctx.platform->GetBackend();
        imgui_event_token_ = platform_backend.GetSdlEventProcessors().Register(
            [this](void* sdl_event) {
                if (ctx_.imgui_system && ctx_.imgui_system->IsInitialized()) {
                    ctx_.imgui_system->ProcessSDLEvent(sdl_event);
                }
            });
    }

    // ── Flush pipeline cache (warm data persists across runs) ──
    ctx_.shader_manager->FlushCache();

    // ── Start shader file watching (watches all registered slang directories) ──
    ctx_.shader_watcher = std::make_unique<ShaderSystem::ShaderWatcher>(*ctx_.shader_manager);
    ctx_.shader_watcher->Start();

    return true;
}

std::vector<GameEngine::UploadedMesh> GameEngine::UploadScene(
    VulkanEngine::Application::ApplicationContext& /*ctx*/,
    const std::vector<VulkanEngine::GpuResources::MeshData>& meshes) {
    std::vector<UploadedMesh> result;

    if (!ctx_.mesh_manager) return result;

    std::vector<VulkanEngine::SubMesh> all_submeshes;
    std::unordered_set<std::uint32_t> vertex_buffers_updated;
    std::unordered_set<std::uint32_t> index_buffers_updated;

    for (const auto& mesh_data : meshes) {
        auto handle = ctx_.mesh_manager->UploadPersistent(mesh_data);
        if (!handle.IsValid()) {
            LOGIFACE_LOG(error, "UploadScene: failed to upload mesh");
            continue;
        }

        const auto* info = ctx_.mesh_manager->GetMeshInfo(handle);
        if (!info) continue;

        const std::uint32_t index_offset = static_cast<std::uint32_t>(
            info->index_allocation.offset / sizeof(std::uint32_t));

        UploadedMesh uploaded{};
        uploaded.first_submesh = static_cast<std::uint32_t>(all_submeshes.size());
        uploaded.submesh_count = static_cast<std::uint32_t>(info->sub_meshes.size());
        uploaded.vertex_buffer_index = info->vertex_allocation.buffer_index;
        uploaded.index_buffer_index = info->index_allocation.buffer_index;

        if (info->sub_meshes.empty()) {
            const std::uint32_t total_indices = static_cast<std::uint32_t>(
                info->index_allocation.size / sizeof(std::uint32_t));
            SubMesh default_sm{};
            default_sm.index_start = index_offset;
            default_sm.index_count = total_indices;
            all_submeshes.push_back(default_sm);
            uploaded.submesh_count = 1;
        } else {
            for (const auto& sm : info->sub_meshes) {
                auto adjusted = sm;
                adjusted.index_start += index_offset;
                all_submeshes.push_back(adjusted);
            }
        }

        result.push_back(uploaded);

        // Update SceneRenderer descriptors for new heap blocks (all frames)
        if (vertex_buffers_updated.insert(info->vertex_allocation.buffer_index).second) {
            if (info->vertex_allocation.buffer_index < ctx_.vertex_heap.GetBufferCount()) {
                ctx_.scene_renderer->UpdateAllFrameVertexBufferArrayElements(
                    info->vertex_allocation.buffer_index,
                    ctx_.vertex_heap.GetBuffer(info->vertex_allocation.buffer_index),
                    ctx_.vertex_heap.GetConfig().block_size);
            }
        }
        if (index_buffers_updated.insert(info->index_allocation.buffer_index).second) {
            if (info->index_allocation.buffer_index < ctx_.index_heap.GetBufferCount()) {
                ctx_.scene_renderer->UpdateAllFrameIndexBufferArrayElements(
                    info->index_allocation.buffer_index,
                    ctx_.index_heap.GetBuffer(info->index_allocation.buffer_index),
                    ctx_.index_heap.GetConfig().block_size);
            }
        }
    }

    ctx_.scene_renderer->SetSubmeshes(all_submeshes);
    scene_valid_ = true;

    return result;
}

std::vector<GameEngine::UploadedMesh> GameEngine::UploadSceneFromFiles(
    VulkanEngine::Application::ApplicationContext& ctx,
    const std::vector<std::filesystem::path>& file_paths,
    const std::vector<SceneLoader::MaterialId>* material_bindings) {
    std::vector<VulkanEngine::GpuResources::MeshData> mesh_data_list;
    mesh_data_list.reserve(file_paths.size());

    for (const auto& path : file_paths) {
        auto md = SceneLoader::LoadMeshData(path, material_bindings);
        mesh_data_list.push_back(std::move(md));
    }

    return UploadScene(ctx, mesh_data_list);
}

Components::Camera& GameEngine::CreateCamera(ComponentRegistry& registry) {
    auto& entity = registry.CreateEntity();
    registry.AddComponent<Components::Camera>(entity);
    camera_ = entity.GetComponent<Components::Camera>();
    return *camera_;
}

void GameEngine::FrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx) {
    ctx_.component_registry.UpdateAllComponentsAsync(ctx.frame.delta_time);

    if (!ctx_.mesh_manager) {
        LOGIFACE_LOG(warn, "Game::FrameUpdate: mesh_manager is null, skipping ProcessFrame");
        return;
    }
    if (!scene_valid_) {
        LOGIFACE_LOG(warn, "Game::FrameUpdate: scene_valid_ is false, skipping ProcessFrame");
        return;
    }

    {
        // Index every engine FIF ring (dynamic mesh uploads, per-frame descriptors,
        // frame block arrays) with the monotonic frame counter modulo the pipeline
        // depth. Image indices must never drive FIF slot selection.
        const std::uint32_t fif = ctx.bootstrap->GetBackend().GetFramesInFlight();
        const std::uint32_t frame_index = ctx.frame.frame_counter % fif;
        ctx_.mesh_render_system.ProcessFrame(
            ctx_.component_registry,
            ctx_.mesh_registry,
            *ctx_.mesh_manager,
            *ctx_.scene_renderer,
            ctx_.vertex_heap,
            ctx_.index_heap,
            ctx_.material_mgr,
            frame_index);
    }
}

void GameEngine::FrameRender(const VulkanEngine::Application::ApplicationContext& ctx) {
    if (!ctx_.renderer) {
        LOGIFACE_LOG(warn, "Game::FrameRender: renderer is null, skipping");
        return;
    }
    if (!camera_) {
        LOGIFACE_LOG(warn, "Game::FrameRender: camera_ is null, skipping");
        return;
    }
    if (!scene_valid_) {
        LOGIFACE_LOG(debug, "Game::FrameRender: scene_valid_ is false, skipping");
        return;
    }

    if (ctx_.mesh_manager) {
        const std::uint32_t fif = ctx.bootstrap->GetBackend().GetFramesInFlight();
        ctx_.mesh_manager->EndFrame(ctx.frame.frame_counter % fif);
    }

    // Flush dirty material data to GPU before rendering
    ctx_.material_mgr.FlushDirtyMaterials();

    // Rebuild technique pipelines whose shaders changed (hot reload)
    const auto frame_index = ctx.bootstrap->GetSnapshot().frame_index;
    ctx_.technique_mgr->PollShaders(*ctx_.shader_manager, *ctx_.pipeline_factory,
                                    frame_index);

#ifdef VKENGINE_PHYSICAL_CAMERA
    if (ctx_.physical_camera) {
        ctx_.physical_camera->PollShaders(frame_index);
    }
#endif

    ctx_.renderer->RenderFrame(*ctx.bootstrap,
                               ctx_.component_registry,
                               *camera_,
                               *ctx_.technique_mgr,
                               *ctx_.bindless_mgr,
                               *ctx_.scene_renderer,
                               ctx_.imgui_system.get(),
                               ctx.frame.image_index
#ifdef VKENGINE_PHYSICAL_CAMERA
                               , ctx_.physical_camera.get()
#endif
                               );
}

void GameEngine::Shutdown() {
    if (!initialized_) return;

    if (vk_backend_) {
        bootstrap_.Shutdown(ctx_, *vk_backend_);
    }

    imgui_event_token_ = {};
#ifdef VKENGINE_PHYSICAL_CAMERA
    physical_camera_sdl_token_ = {};
#endif

    if (scene_valid_) {
        scene_valid_ = false;
    }

    initialized_ = false;
    vk_backend_ = nullptr;
}

} // namespace VulkanEngine
