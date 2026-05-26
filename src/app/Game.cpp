module;

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/vec4.hpp> //NOLINT(misc-include-cleaner)
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <filesystem>

#include <SDL3/SDL_keycode.h>
#include <imgui.h>
#include <vulkan/vulkan.hpp>

module App.Game;

import VulkanEngine.Application;
import VulkanEngine.DefaultRenderer;
import VulkanEngine.SceneLoader;
import VulkanEngine.TechniqueManager;
import VulkanEngine.MaterialManager;
import VulkanEngine.SceneRenderer;
import VulkanEngine.Components.Camera;
import VulkanEngine.Components.Transform;
import VulkanEngine.Components.MeshReference;
import VulkanEngine.StandardMeshPipeline;
import VulkanEngine.ImGuiSystem;
import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;
import VulkanEngine.DefaultResources;
import VulkanEngine.ShaderLoader;
import VulkanEngine.Input;
import VulkanEngine.GpuResources;
import App.Components.DemoInputComponent;

namespace App::Game {

DemoGame::DemoGame(const RenderMode render_mode, const std::filesystem::path& executable_path,
                   std::filesystem::path model_path,
                   std::filesystem::path texture_path)
    : render_mode_(render_mode)
    , exe_dir_(executable_path.parent_path())
    , model_path_(std::move(model_path))
    , texture_path_(std::move(texture_path)) {
    hooks_.on_setup = [this](VulkanEngine::Application::ApplicationContext& ctx) -> bool {
        return OnSetup(ctx);
    };
    hooks_.on_pre_input = [this](VulkanEngine::Application::ApplicationContext& ctx) {
        OnPreInput(ctx);
    };
    hooks_.should_filter_mouse_input = [this]() -> bool {
        return ShouldFilterMouseInput();
    };
    hooks_.should_filter_keyboard_input = [this]() -> bool {
        return ShouldFilterKeyboardInput();
    };
    hooks_.on_frame_update = [this](VulkanEngine::Application::ApplicationContext& ctx) {
        OnFrameUpdate(ctx);
    };
    hooks_.on_frame_render = [this](VulkanEngine::Application::ApplicationContext& ctx) {
        OnFrameRender(ctx);
    };
    hooks_.on_shutdown = [this](VulkanEngine::Application::ApplicationContext& ctx) {
        OnShutdown(ctx);
    };
}

DemoGame::~DemoGame() = default;

bool DemoGame::OnSetup(VulkanEngine::Application::ApplicationContext& ctx) {
    missing_texture_ = VulkanEngine::DefaultResources::DefaultResources::CreateCheckerboard(resource_manager_);
    fallback_handle_ = VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>("checkerboard_default", &resource_manager_);

    VulkanEngine::ResourceHandle<VulkanEngine::TextureResource> tex_handle;
    if (!texture_path_.empty()) {
        tex_handle = VulkanEngine::SceneLoader::SceneManager::LoadTextureFromPath(resource_manager_, texture_path_, fallback_handle_);
    } else {
        tex_handle = VulkanEngine::SceneLoader::SceneManager::LoadTexture(resource_manager_, exe_dir_ / "textures", fallback_handle_);
    }

    const std::filesystem::path shader_dir = SHADER_DIR;

    std::string frag_name;
    switch (render_mode_) {
        case RenderMode::Normals:
            frag_name = "normals.frag.spv";
            break;
        case RenderMode::NoTextures:
            frag_name = "solid.frag.spv";
            break;
        default:
            frag_name = "standard_mesh.frag.spv";
            break;
    }

    auto vert_spv = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(shader_dir / "main_world.vert.spv");
    auto frag_spv = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(shader_dir / frag_name);

    bindless_mgr_ = std::make_unique<VulkanEngine::BindlessManager::BindlessManager>();
    if (!bindless_mgr_->Initialize(ctx.bootstrap->GetBackend())) {
        return false;
    }

    auto upload_to_bindless = [&](VulkanEngine::TextureResource* tex) -> uint32_t {
        if (!tex || !tex->HasPixels()) return 0;
        auto gpu_tex = VulkanEngine::GpuResources::GpuTexture::CreateFromPixels(
            ctx.bootstrap->GetBackend(),
            reinterpret_cast<const uint8_t*>(tex->GetPixels().data()),
            tex->GetWidth(), tex->GetHeight());
        if (gpu_tex.IsValid()) {
            return bindless_mgr_->AllocateTextureSlot(std::move(gpu_tex));
        }
        return 0;
    };

    const uint32_t fallback_slot = upload_to_bindless(missing_texture_.get());
    uint32_t active_slot = fallback_slot;
    if (tex_handle.IsValid() && tex_handle->HasPixels()) {
        const uint32_t tex_slot = upload_to_bindless(tex_handle.Get());
        if (tex_slot != 0) {
            active_slot = tex_slot;
        }
    }

    // Load meshes
    std::vector<VulkanEngine::SceneLoader::LoadedMeshData> meshes;
    std::vector<std::string> mesh_names;

    if (!model_path_.empty()) {
        auto mesh_data = VulkanEngine::SceneLoader::SceneManager::LoadMeshFromFilePath(model_path_);
        meshes.push_back(std::move(mesh_data));
        mesh_names.push_back(model_path_.stem().string());
    } else {
        [[maybe_unused]] const bool meshes_loaded = VulkanEngine::SceneLoader::SceneManager::LoadAllMeshes(exe_dir_ / "models", meshes, mesh_names);
    }

    if (meshes.empty()) {
        meshes.push_back(VulkanEngine::SceneLoader::SceneManager::CreateFallbackQuad());
        mesh_names.emplace_back("fallback_quad");
    }

    // Compute total vertex and index counts
    uint32_t total_vertex_count = 0;
    uint32_t total_index_count = 0;
    for (const auto& m : meshes) {
        total_vertex_count += static_cast<uint32_t>(m.positions.size() / 3);
        total_index_count += static_cast<uint32_t>(m.indices.size());
    }

    auto& backend = ctx.bootstrap->GetBackend();

    // Initialize staging and heaps
    VulkanEngine::GpuResources::HeapConfig heap_config{};
    heap_config.block_size = ctx.geometry_buffer_size_mb << 20; // Convert MB to bytes
    if (!vertex_heap_.Initialize(backend, heap_config, "vertex")) {
        return false;
    }
    if (!index_heap_.Initialize(backend, heap_config, "index")) {
        return false;
    }
    if (!staging_mgr_.Initialize(backend)) {
        return false;
    }

    // Create SceneRenderer with known counts
    scene_renderer_ = std::make_unique<VulkanEngine::SceneRenderer::SceneRenderer>();
    if (!scene_renderer_->Initialize(backend, vertex_heap_, total_vertex_count, total_index_count)) {
        return false;
    }

    // Create TechniqueManager and register standard technique
    technique_mgr_ = std::make_unique<VulkanEngine::TechniqueManager::TechniqueManager>();
    VulkanEngine::StandardMeshPipeline::PipelineConfig pipeline_config{};
    pipeline_config.depth_test_enable = true;
    pipeline_config.depth_write_enable = true;
    pipeline_config.depth_compare_op = vk::CompareOp::eLessOrEqual;
    const uint16_t main_technique = technique_mgr_->RegisterTechnique(
        *ctx.bootstrap, vert_spv, frag_spv, pipeline_config,
        bindless_mgr_->GetLayout(),
        scene_renderer_->GetSubmeshVertexDataLayout(),
        scene_renderer_->GetRawVertexLayout(),
        scene_renderer_->GetIndirectionLayout());

    // Initialize MaterialManager singleton and register fallback material
    VulkanEngine::MaterialManager::MaterialManager::Initialize();
    VulkanEngine::MaterialManager::MaterialDefinition fallback_def{};
    fallback_def.technique_id = VulkanEngine::TechniqueId{main_technique};
    fallback_def.texture_slot = VulkanEngine::TextureSlot{static_cast<uint16_t>(active_slot)};
    [[maybe_unused]] const auto fallback_mat = VulkanEngine::MaterialManager::MaterialManager::Get().RegisterMaterial(fallback_def);

    // Upload combined scene
    combined_scene_ = VulkanEngine::SceneLoader::SceneManager::UploadCombined(
        *ctx.bootstrap, staging_mgr_, vertex_heap_, index_heap_,
        meshes);

    if (!combined_scene_.vertex_allocation.IsValid() || !combined_scene_.index_allocation.IsValid()) {
        return false;
    }

    // Assign technique and texture slot per-mesh
    for (size_t i = 0; i < combined_scene_.meshes.size(); ++i) {
        const auto& mi = combined_scene_.meshes[i];
        const uint16_t slot = (i == 0) ? static_cast<uint16_t>(active_slot)
                                       : static_cast<uint16_t>(fallback_slot);
        for (uint32_t s = 0; s < mi.submesh_count; ++s) {
            auto& sm = combined_scene_.submeshes[mi.first_submesh_index + s];
            if (sm.technique_id.value == 0 && sm.texture_slot.value == 0) {
                sm.technique_id = VulkanEngine::TechniqueId{main_technique};
                sm.texture_slot = VulkanEngine::TextureSlot{slot};
            }
        }
    }
    scene_renderer_->SetSubmeshes(combined_scene_.submeshes);

    // Update bindless vertex array with the heap buffer
    if (combined_scene_.vertex_allocation.buffer_index < vertex_heap_.GetBufferCount()) {
        scene_renderer_->UpdateVertexBufferArrayElement(
            combined_scene_.vertex_allocation.buffer_index,
            vertex_heap_.GetBuffer(combined_scene_.vertex_allocation.buffer_index),
            vertex_heap_.GetConfig().block_size);
    }

    // Update bindless index array with the heap buffer
    if (combined_scene_.index_allocation.buffer_index < index_heap_.GetBufferCount()) {
        scene_renderer_->UpdateIndexBufferArrayElement(
            combined_scene_.index_allocation.buffer_index,
            index_heap_.GetBuffer(combined_scene_.index_allocation.buffer_index),
            index_heap_.GetConfig().block_size);
    }

    // Create default renderer
    VulkanEngine::DefaultRenderer::DefaultRendererConfig renderer_config{};
    renderer_config.enable_imgui = true;
    renderer_config.clear_color = {0.1f, 0.1f, 0.1f, 1.0f};

    renderer_ = std::make_unique<VulkanEngine::DefaultRenderer::DefaultRenderer>();
    if (!renderer_->Initialize(*ctx.bootstrap, renderer_config)) {
        return false;
    }
    scene_valid_ = true;

    // Setup ImGui
    imgui_backend_ = VulkanEngine::Backend::ImGui::CreateImGuiBackend();
    imgui_system_ = std::make_unique<VulkanEngine::ImGuiSystem::ImGuiSystem>(imgui_backend_);

    const auto surface_format = backend.GetSurfaceFormat();

    VulkanEngine::Backend::ImGui::ImGuiBackendConfig imgui_backend_config{};
    imgui_backend_config.image_count = ctx.bootstrap->GetSnapshot().swapchain_image_count;
    imgui_backend_config.swapchain_format = static_cast<vk::Format>(surface_format.format);

    VulkanEngine::ImGuiSystem::ImGuiSystemInitInfo imgui_init_info{};
    imgui_init_info.sdl_window = ctx.window;
    imgui_init_info.backend_config = imgui_backend_config;
    imgui_init_info.instance = backend.GetInstance();
    imgui_init_info.physical_device = backend.GetPhysicalDevice();
    imgui_init_info.device = backend.GetDevice();
    imgui_init_info.queue_family = backend.GetGraphicsQueueFamily();
    imgui_init_info.queue = backend.GetGraphicsQueue();
    imgui_init_info.api_version = VK_API_VERSION_1_3;

    if (!imgui_system_->Initialize(imgui_init_info)) {
        return false;
    }

    auto& platform_backend = ctx.platform->GetBackend();
    platform_backend.SetEventProcessor(
        [](void* user_data, void* sdl_event) {
            auto* imgui_sys = static_cast<VulkanEngine::ImGuiSystem::ImGuiSystem*>(user_data);
            if (imgui_sys && imgui_sys->IsInitialized()) {
                imgui_sys->ProcessSDLEvent(sdl_event);
            }
        },
        imgui_system_.get());

    // Create camera entity
    auto& component_registry = backend.GetComponentRegistry();
    auto& camera_entity = component_registry.CreateEntity();
    component_registry.AddComponent<VulkanEngine::Components::Camera>(camera_entity);

    // Create game entities (one per mesh, each may expand into multiple submeshes)
    for (size_t i = 0; i < combined_scene_.meshes.size(); ++i) {
        auto& entity = component_registry.CreateEntity();
        component_registry.AddComponent<VulkanEngine::Components::Transform>(entity);

        auto& mesh_ref = component_registry.AddComponent<VulkanEngine::Components::MeshReference>(entity);
        mesh_ref.first_submesh = combined_scene_.meshes[i].first_submesh_index;
        mesh_ref.submesh_count = combined_scene_.meshes[i].submesh_count;
        mesh_ref.index_buffer_index = static_cast<uint8_t>(combined_scene_.index_allocation.buffer_index);

        if (i == 0) {
            component_registry.AddComponent<App::Components::DemoInputComponent>(entity, ctx.input_system);
        }
    }

    component_registry.InitializeAllComponents();

    camera_ = camera_entity.GetComponent<VulkanEngine::Components::Camera>();

    ctx.quit_action_handle = ctx.input_system->BindAction("quit", VulkanEngine::Input::InputBinding::Key(SDLK_ESCAPE));

    return true;
}

void DemoGame::OnPreInput(VulkanEngine::Application::ApplicationContext& /*ctx*/) {
}

bool DemoGame::ShouldFilterMouseInput() {
    return ImGui::GetIO().WantCaptureMouse;
}

bool DemoGame::ShouldFilterKeyboardInput() {
    return ImGui::GetIO().WantCaptureKeyboard;
}

void DemoGame::OnFrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx) {
    ctx.bootstrap->GetBackend().GetComponentRegistry().UpdateAllComponentsAsync(ctx.frame.delta_time);
}

void DemoGame::OnFrameRender(const VulkanEngine::Application::ApplicationContext& ctx) {
    if (!renderer_ || !camera_ || !scene_valid_) {
        return;
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

void DemoGame::OnShutdown(VulkanEngine::Application::ApplicationContext& /*ctx*/) {
    if (renderer_) {
        renderer_->Shutdown();
        renderer_.reset();
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
    VulkanEngine::MaterialManager::MaterialManager::Shutdown();
    if (technique_mgr_) {
        technique_mgr_->Shutdown();
        technique_mgr_.reset();
    }
    if (scene_valid_) {
        combined_scene_.meshes.clear();
        scene_valid_ = false;
    }

    if (imgui_system_) {
        imgui_system_->Shutdown();
        imgui_system_.reset();
    }
}

}
