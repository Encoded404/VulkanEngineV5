module;

#include <memory>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <cstdint>

#include <vulkan/vulkan.hpp>
#include <logging/logging.hpp>

module VulkanEngine.Game;

namespace VulkanEngine::Game {

GameEngine::~GameEngine() {
    if (initialized_) {
        Shutdown();
    }
}

bool GameEngine::Setup(VulkanEngine::Application::ApplicationContext& ctx, const GameConfig& config) {
    bootstrap_ = ctx.bootstrap;
    config_ = config;
    auto& backend = ctx.bootstrap->GetBackend();

    missing_texture_ = DefaultTextureFactory::DefaultTextureFactory::CreateCheckerboard(resource_manager_);
    fallback_handle_ = ResourceHandle<TextureResource>("checkerboard_default", &resource_manager_);

    vert_spv_holder_ = ShaderLoader::ShaderLoader::LoadSpirv(
        config.shader_dir / config.vertex_shader_file);
    frag_spv_holder_ = ShaderLoader::ShaderLoader::LoadSpirv(
        config.shader_dir / config.fragment_shader_file);

    bindless_mgr_ = std::make_unique<BindlessManager::BindlessManager>();
    if (!bindless_mgr_->Initialize(backend)) {
        return false;
    }

    GpuResources::HeapConfig heap_config{};
    heap_config.block_size = ctx.geometry_buffer_size_mb << 20;
    if (!vertex_heap_.Initialize(backend, heap_config, "vertex")) return false;
    if (!index_heap_.Initialize(backend, heap_config, "index")) return false;
    if (!staging_mgr_.Initialize(backend)) return false;

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
        return bindless_mgr_->AllocateTextureSlot(std::move(gpu_tex));
    }
    return 0;
}

uint32_t GameEngine::LoadTexture(VulkanEngine::Application::ApplicationContext& ctx, const std::filesystem::path& path) {
    auto tex_handle = SceneLoader::SceneLoader::LoadTextureFromPath(
        resource_manager_, path, fallback_handle_);
    if (tex_handle.IsValid() && tex_handle->HasPixels()) {
        return UploadTextureToBindless(ctx, tex_handle.Get());
    }
    return 0;
}

std::vector<SceneLoader::LoadedMeshData> GameEngine::LoadMeshes(
    const std::vector<std::filesystem::path>& paths) {
    std::vector<SceneLoader::LoadedMeshData> meshes;
    meshes.reserve(paths.size());
    for (const auto& path : paths) {
        meshes.push_back(SceneLoader::SceneLoader::LoadMeshFromFilePath(path));
    }
    if (meshes.empty()) {
        meshes.push_back(SceneLoader::SceneLoader::CreateFallbackQuad());
    }
    return meshes;
}

std::vector<SceneLoader::LoadedMeshData> GameEngine::LoadMeshDirectory(
    const std::filesystem::path& dir) {
    std::vector<SceneLoader::LoadedMeshData> meshes;
    std::vector<std::string> names;
    [[maybe_unused]] const bool loaded = SceneLoader::SceneLoader::LoadAllMeshes(dir, meshes, names);
    if (meshes.empty()) {
        meshes.push_back(SceneLoader::SceneLoader::CreateFallbackQuad());
    }
    return meshes;
}

const SceneLoader::CombinedScene& GameEngine::FinalizeScene(
    VulkanEngine::Application::ApplicationContext& ctx,
    const std::vector<SceneLoader::LoadedMeshData>& meshes) {
    auto& backend = ctx.bootstrap->GetBackend();

    uint32_t total_vertex_count = 0;
    uint32_t total_index_count = 0;
    for (const auto& m : meshes) {
        total_vertex_count += static_cast<uint32_t>(m.positions.size() / 3);
        total_index_count += static_cast<uint32_t>(m.indices.size());
    }

    scene_renderer_ = std::make_unique<SceneRenderer::SceneRenderer>();
    scene_renderer_->Initialize(backend, vertex_heap_, total_vertex_count, total_index_count);

    technique_mgr_ = std::make_unique<TechniqueManager::TechniqueManager>();
    main_technique_id_ = technique_mgr_->RegisterTechnique(
        *ctx.bootstrap, vert_spv_holder_, frag_spv_holder_,
        config_.pipeline_config,
        bindless_mgr_->GetLayout(),
        scene_renderer_->GetSubmeshVertexDataLayout(),
        scene_renderer_->GetRawVertexLayout(),
        scene_renderer_->GetIndirectionLayout());

    scene_renderer_->SetupTechniqueDgcCallback(*technique_mgr_);

    MaterialManager::MaterialManager::Initialize();

    combined_scene_ = SceneLoader::SceneLoader::UploadCombined(
        *ctx.bootstrap, staging_mgr_, vertex_heap_, index_heap_, meshes);

    if (combined_scene_.vertex_allocation.IsValid() && combined_scene_.index_allocation.IsValid()) {
        scene_valid_ = true;

        // Resolve materials for all submeshes
        uint32_t fallback_slot = UINT32_MAX;
        std::map<std::filesystem::path, uint32_t> loaded_textures;
        std::map<std::pair<std::filesystem::path, uint16_t>, MaterialId> material_cache;

        uint32_t flat_idx = 0;
        for (size_t mi = 0; mi < meshes.size(); ++mi) {
            for (size_t si = 0; si < meshes[mi].submeshes.size(); ++si) {
                if (flat_idx >= combined_scene_.submeshes.size()) break;

                const auto& desc = (si < meshes[mi].submesh_materials.size())
                    ? meshes[mi].submesh_materials[si]
                    : SceneLoader::MaterialDescriptor{};
                auto& sm = combined_scene_.submeshes[flat_idx];
                ++flat_idx;

                auto cache_key = std::make_pair(desc.texture_path, desc.technique_hint);
                auto cache_it = material_cache.find(cache_key);
                if (cache_it != material_cache.end()) {
                    sm.material_id = cache_it->second;
                    continue;
                }

                uint32_t tex_slot = 0;
                if (!desc.texture_path.empty()) {
                    auto tex_it = loaded_textures.find(desc.texture_path);
                    if (tex_it != loaded_textures.end()) {
                        tex_slot = tex_it->second;
                    } else {
                        uint32_t slot = LoadTexture(ctx, desc.texture_path);
                        if (slot != 0) {
                            tex_slot = slot;
                            loaded_textures[desc.texture_path] = slot;
                        }
                    }
                }

                if (tex_slot == 0) {
                    if (fallback_slot == UINT32_MAX) {
                        fallback_slot = UploadTextureToBindless(ctx, missing_texture_.get());
                    }
                    tex_slot = fallback_slot;
                }

                uint16_t tech_id = main_technique_id_;
                MaterialManager::MaterialDefinition mat_def{};
                mat_def.technique_id = TechniqueManager::TechniqueId{tech_id};
                mat_def.texture_slot = BindlessManager::TextureSlot{static_cast<uint16_t>(tex_slot)};
                auto mat_id = MaterialManager::MaterialManager::Get().RegisterMaterial(mat_def);

                material_cache[cache_key] = mat_id;
                sm.material_id = mat_id;
            }
        }

        scene_renderer_->SetSubmeshes(combined_scene_.submeshes);

        if (combined_scene_.vertex_allocation.buffer_index < vertex_heap_.GetBufferCount()) {
            scene_renderer_->UpdateVertexBufferArrayElement(
                combined_scene_.vertex_allocation.buffer_index,
                vertex_heap_.GetBuffer(combined_scene_.vertex_allocation.buffer_index),
                vertex_heap_.GetConfig().block_size);
        }

        if (combined_scene_.index_allocation.buffer_index < index_heap_.GetBufferCount()) {
            scene_renderer_->UpdateIndexBufferArrayElement(
                combined_scene_.index_allocation.buffer_index,
                index_heap_.GetBuffer(combined_scene_.index_allocation.buffer_index),
                index_heap_.GetConfig().block_size);
        }
    }

    renderer_ = std::make_unique<Renderer::Renderer>();
    renderer_->Initialize(*ctx.bootstrap, config_.renderer_config);

    if (config_.enable_imgui) {
        imgui_backend_ = Backend::ImGui::CreateImGuiBackend();
        imgui_system_ = std::make_unique<ImGui::ImGuiSystem>(imgui_backend_);

        const auto surface_format = backend.GetSurfaceFormat();
        Backend::ImGui::ImGuiBackendConfig imgui_backend_config{};
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
        imgui_init_info.api_version = VK_API_VERSION_1_3;

        [[maybe_unused]] const bool imgui_ok = imgui_system_->Initialize(imgui_init_info);

        auto& platform_backend = ctx.platform->GetBackend();
        platform_backend.SetEventProcessor(
            [](void* user_data, void* sdl_event) {
                auto* imgui_sys = static_cast<ImGui::ImGuiSystem*>(user_data);
                if (imgui_sys && imgui_sys->IsInitialized()) {
                    imgui_sys->ProcessSDLEvent(sdl_event);
                }
            },
            imgui_system_.get());
    }

    return combined_scene_;
}

Components::Camera& GameEngine::CreateCamera(ComponentRegistry& registry) {
    auto& entity = registry.CreateEntity();
    registry.AddComponent<Components::Camera>(entity);
    camera_ = entity.GetComponent<Components::Camera>();
    return *camera_;
}

void GameEngine::FrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx) {
    ctx.bootstrap->GetBackend().GetComponentRegistry().UpdateAllComponentsAsync(ctx.frame.delta_time);
}

void GameEngine::FrameRender(const VulkanEngine::Application::ApplicationContext& ctx) {
    if (!renderer_ || !camera_ || !scene_valid_) return;

    renderer_->RenderFrame(*ctx.bootstrap,
                           ctx.bootstrap->GetBackend().GetComponentRegistry(),
                           *camera_,
                           *technique_mgr_,
                           *bindless_mgr_,
                           *scene_renderer_,
                           imgui_system_.get(),
                           ctx.frame.image_index);
}

void GameEngine::Shutdown() {
    if (!initialized_) return;

    if (bootstrap_) {
        try {
            bootstrap_->GetBackend().GetDevice().waitIdle();
        } catch (...) { 
            LOGIFACE_LOG(warn, "Exception during GPU wait idle in Game shutdown");
        }
    }

    if (renderer_) {
        renderer_->Shutdown();
        renderer_.reset();
    }
    if (imgui_system_) {
        imgui_system_->Shutdown();
        imgui_system_.reset();
    }
    if (scene_renderer_) {
        scene_renderer_->Shutdown();
        scene_renderer_.reset();
    }
    staging_mgr_.Shutdown();
    vertex_heap_.Shutdown();
    index_heap_.Shutdown();
    if (bindless_mgr_) {
        bindless_mgr_->Shutdown();
        bindless_mgr_.reset();
    }
    MaterialManager::MaterialManager::Shutdown();
    if (technique_mgr_) {
        technique_mgr_->Shutdown();
        technique_mgr_.reset();
    }
    if (scene_valid_) {
        combined_scene_.meshes.clear();
        scene_valid_ = false;
    }

    initialized_ = false;
    bootstrap_ = nullptr;
}

} // namespace VulkanEngine::Game

