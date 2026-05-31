module;

#include <memory>
#include <vector>
#include <cstdint>
#include <unordered_set>

#include <vulkan/vulkan.hpp>
#include <logging/logging.hpp>

module VulkanEngine.Game;

import Shaders.Engine.MainIndirVert;
import Shaders.Engine.StandardMeshFrag;
import VulkanEngine.MeshManager;

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
    fallback_handle_ = ResourceHandle<TextureResource>(ResourceId{"checkerboard_default"}, &resource_manager_);

    vert_spv_holder_ = std::vector<std::uint32_t>{
        Shaders::Engine::MainIndirVert::GetSpirvWords().begin(),
        Shaders::Engine::MainIndirVert::GetSpirvWords().end()};
    frag_spv_holder_ = std::vector<std::uint32_t>{
        Shaders::Engine::StandardMeshFrag::GetSpirvWords().begin(),
        Shaders::Engine::StandardMeshFrag::GetSpirvWords().end()};

    bindless_mgr_ = std::make_unique<BindlessManager::BindlessManager>();
    if (!bindless_mgr_->Initialize(backend)) {
        return false;
    }

    GpuResources::HeapConfig heap_config{};
    heap_config.block_size = ctx.geometry_buffer_size_mb << 20;
    if (!vertex_heap_.Initialize(backend, heap_config, "vertex")) return false;
    if (!index_heap_.Initialize(backend, heap_config, "index")) return false;
    if (!staging_mgr_.Initialize(backend)) return false;

    {
        GpuResources::HeapConfig dynamic_heap_config{};
        dynamic_heap_config.block_size = 32ULL << 20;
        dynamic_heap_config.memory_flags =
            vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent;

        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT_DYN; ++i) {
            if (!dynamic_vertex_heaps_[i].Initialize(backend, dynamic_heap_config,
                "dynamic_vertex_fifo" + std::to_string(i))) return false;
            if (!dynamic_index_heaps_[i].Initialize(backend, dynamic_heap_config,
                "dynamic_index_fifo" + std::to_string(i))) return false;
        }
    }

    mesh_manager_ = std::make_unique<MeshManager>();
    if (!mesh_manager_->Initialize(backend, &vertex_heap_, &index_heap_,
                                   &staging_mgr_,
                                   dynamic_vertex_heaps_.data(),
                                   dynamic_index_heaps_.data(),
                                   FRAMES_IN_FLIGHT_DYN)) {
        return false;
    }

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
        return bindless_mgr_->AllocateTextureSlot(std::move(gpu_tex), tex->GetId());
    }
    LOGIFACE_LOG(debug, "Failed to create GPU texture for: " + tex->GetId().value + ", using fallback");
    return 0;
}

uint32_t GameEngine::LoadTexture(VulkanEngine::Application::ApplicationContext& ctx, const std::filesystem::path& path) {
    auto tex_handle = SceneLoader::SceneLoader::LoadTextureFromPath(
        resource_manager_, path, fallback_handle_);
    if (tex_handle.IsValid() && tex_handle->HasPixels()) {
        return UploadTextureToBindless(ctx, tex_handle.Get());
    }
    LOGIFACE_LOG(debug, "Failed to load texture from path: " + path.string() + ", using fallback");
    return 0;
}

bool GameEngine::InitRenderer(VulkanEngine::Application::ApplicationContext& ctx,
                              std::span<const std::uint32_t> vert_override,
                              std::span<const std::uint32_t> frag_override) {
    auto& backend = ctx.bootstrap->GetBackend();

    constexpr uint32_t initial_indirection_entries = 1u << 20; // 1M entries = 8MB
    scene_renderer_ = std::make_unique<SceneRenderer::SceneRenderer>();
    if (!scene_renderer_->Initialize(backend, vertex_heap_, initial_indirection_entries)) {
        LOGIFACE_LOG(error, "SceneRenderer::Initialize failed");
        return false;
    }

    technique_mgr_ = std::make_unique<TechniqueManager::TechniqueManager>();
    {
        auto resolve_spv = [](const std::span<const std::uint32_t>& override_spv,
                              const std::vector<std::uint32_t>& default_spv) {
            if (!override_spv.empty()) {
                return std::vector<std::uint32_t>{override_spv.begin(), override_spv.end()};
            }
            return default_spv;
        };
        auto vert = resolve_spv(vert_override, vert_spv_holder_);
        auto frag = resolve_spv(frag_override, frag_spv_holder_);
        main_technique_id_ = technique_mgr_->RegisterTechnique(
            *ctx.bootstrap, vert, frag, config_.pipeline_config,
        bindless_mgr_->GetLayout(),
        scene_renderer_->GetSubmeshVertexDataLayout(),
        scene_renderer_->GetRawVertexLayout(),
        scene_renderer_->GetIndirectionLayout());
    }

    scene_renderer_->SetupTechniqueDgcCallback(*technique_mgr_);

    MaterialManager::MaterialManager::Initialize();

    // Register fallback material (ID 0): main technique, bindless checkerboard
    const uint32_t fallback_slot = UploadTextureToBindless(ctx, missing_texture_.get());
    MaterialManager::MaterialDefinition fallback_def{};
    fallback_def.technique_id = TechniqueManager::TechniqueId{main_technique_id_};
    fallback_def.texture_slot = BindlessManager::TextureSlot{static_cast<uint16_t>(fallback_slot)};
    fallback_def.blend_mode = MaterialManager::BlendMode::Opaque;
    [[maybe_unused]] const auto fallback_mat_id = MaterialManager::MaterialManager::Get().RegisterMaterial(fallback_def, resource_manager_, *bindless_mgr_);

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
        imgui_event_token_ = platform_backend.GetSdlEventProcessors().Register(
            [this](void* sdl_event) {
                if (imgui_system_ && imgui_system_->IsInitialized()) {
                    imgui_system_->ProcessSDLEvent(sdl_event);
                }
            });
    }

    return true;
}

std::vector<GameEngine::UploadedMesh> GameEngine::UploadScene(
    VulkanEngine::Application::ApplicationContext& /*ctx*/,
    const std::vector<VulkanEngine::GpuResources::MeshData>& meshes) {
    std::vector<UploadedMesh> result;

    if (!mesh_manager_) return result;

    std::vector<VulkanEngine::SubMesh> all_submeshes;
    std::unordered_set<uint32_t> vertex_buffers_updated;
    std::unordered_set<uint32_t> index_buffers_updated;

    for (const auto& mesh_data : meshes) {
        auto handle = mesh_manager_->UploadPersistent(mesh_data);
        if (!handle.IsValid()) {
            LOGIFACE_LOG(error, "UploadScene: failed to upload mesh");
            continue;
        }

        const auto* info = mesh_manager_->GetMeshInfo(handle);
        if (!info) continue;

        const uint32_t index_offset = static_cast<uint32_t>(
            info->index_allocation.offset / sizeof(uint32_t));

        UploadedMesh uploaded{};
        uploaded.first_submesh = static_cast<uint32_t>(all_submeshes.size());
        uploaded.submesh_count = static_cast<uint32_t>(info->sub_meshes.size());
        uploaded.vertex_buffer_index = info->vertex_allocation.buffer_index;
        uploaded.index_buffer_index = info->index_allocation.buffer_index;

        if (info->sub_meshes.empty()) {
            const uint32_t total_indices = static_cast<uint32_t>(
                info->index_allocation.size / sizeof(uint32_t));
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
            if (info->vertex_allocation.buffer_index < vertex_heap_.GetBufferCount()) {
                scene_renderer_->UpdateAllFrameVertexBufferArrayElements(
                    info->vertex_allocation.buffer_index,
                    vertex_heap_.GetBuffer(info->vertex_allocation.buffer_index),
                    vertex_heap_.GetConfig().block_size);
            }
        }
        if (index_buffers_updated.insert(info->index_allocation.buffer_index).second) {
            if (info->index_allocation.buffer_index < index_heap_.GetBufferCount()) {
                scene_renderer_->UpdateAllFrameIndexBufferArrayElements(
                    info->index_allocation.buffer_index,
                    index_heap_.GetBuffer(info->index_allocation.buffer_index),
                    index_heap_.GetConfig().block_size);
            }
        }
    }

    scene_renderer_->SetSubmeshes(all_submeshes);
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
        auto md = SceneLoader::SceneLoader::LoadMeshData(path, material_bindings);
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
    ctx.bootstrap->GetBackend().GetComponentRegistry().UpdateAllComponentsAsync(ctx.frame.delta_time);

    if (mesh_manager_ && scene_valid_) {
        const uint32_t frame_index = ctx.frame.image_index % 3;
        mesh_render_system_.ProcessFrame(
            ctx.bootstrap->GetBackend().GetComponentRegistry(),
            mesh_registry_,
            *mesh_manager_,
            *scene_renderer_,
            vertex_heap_,
            index_heap_,
            frame_index);
    }
}

void GameEngine::FrameRender(const VulkanEngine::Application::ApplicationContext& ctx) {
    if (!renderer_ || !camera_ || !scene_valid_) return;

    if (mesh_manager_) {
        mesh_manager_->EndFrame(ctx.frame.image_index % 3);
    }

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
    imgui_event_token_ = {};
    if (scene_renderer_) {
        scene_renderer_->Shutdown();
        scene_renderer_.reset();
    }
    mesh_registry_.Shutdown();
    if (mesh_manager_) {
        mesh_manager_->Shutdown();
        mesh_manager_.reset();
    }
    for (auto& heap : dynamic_vertex_heaps_) heap.Shutdown();
    for (auto& heap : dynamic_index_heaps_) heap.Shutdown();
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
        scene_valid_ = false;
    }

    initialized_ = false;
    bootstrap_ = nullptr;
}

} // namespace VulkanEngine::Game

