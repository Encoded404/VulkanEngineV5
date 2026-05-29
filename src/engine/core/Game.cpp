module;

#include <memory>
#include <vector>
#include <cstdint>

#include <vulkan/vulkan.hpp>
#include <logging/logging.hpp>

module VulkanEngine.Game;

import Shaders.Engine;

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
        return bindless_mgr_->AllocateTextureSlot(std::move(gpu_tex), &tex->GetId());
    }
    LOGIFACE_LOG(debug, "Failed to create GPU texture for: " + tex->GetId() + ", using fallback");
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
    [[maybe_unused]] const auto fallback_mat_id = MaterialManager::MaterialManager::Get().RegisterMaterial(fallback_def);

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

const SceneLoader::CombinedScene& GameEngine::UploadScene(
    VulkanEngine::Application::ApplicationContext& ctx,
    const std::vector<SceneLoader::LoadedMeshData>& meshes) {
    combined_scene_ = SceneLoader::SceneLoader::UploadCombined(
        *ctx.bootstrap, staging_mgr_, vertex_heap_, index_heap_, meshes);

    if (combined_scene_.vertex_allocation.IsValid() && combined_scene_.index_allocation.IsValid()) {
        scene_valid_ = true;

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
    imgui_event_token_ = {};
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

