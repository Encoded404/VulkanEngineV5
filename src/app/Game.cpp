module;

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/vec4.hpp>
#include <memory>
#include <string>
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
import VulkanEngine.Components.Material;
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

DemoGame::DemoGame(const std::string& log_level, RenderMode render_mode, const std::filesystem::path& executable_path)
    : render_mode_(render_mode)
    , exe_dir_(executable_path.parent_path()) {
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

    auto* active_texture = missing_texture_.get();
    auto tex_handle = VulkanEngine::SceneLoader::SceneManager::LoadTexture(resource_manager_, exe_dir_ / "textures", fallback_handle_);
    if (tex_handle.IsValid()) {
        active_texture = tex_handle.Get();
    }

    const std::filesystem::path shader_dir = SHADER_DIR;
    std::string frag_name = "standard_mesh.frag.spv";

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
        if (tex_slot != 0 || tex_handle.Get() == active_texture) {
            active_slot = tex_slot > 0 ? tex_slot : active_slot;
        }
    }

    // Load meshes first so we know total vertex count
    std::vector<VulkanEngine::SceneLoader::LoadedMeshData> meshes;
    std::vector<std::string> mesh_names;
    [[maybe_unused]] const bool meshes_loaded = VulkanEngine::SceneLoader::SceneManager::LoadAllMeshes(exe_dir_ / "models", meshes, mesh_names);

    if (meshes.empty()) {
        meshes.push_back(VulkanEngine::SceneLoader::SceneManager::CreateFallbackQuad());
        mesh_names.emplace_back("fallback_quad");
    }

    // Compute total vertex count
    uint32_t total_vertex_count = 0;
    for (const auto& m : meshes) {
        total_vertex_count += static_cast<uint32_t>(m.positions.size() / 3);
    }

    // Create SceneRenderer with known vertex count
    scene_renderer_ = std::make_unique<VulkanEngine::SceneRenderer::SceneRenderer>();
    if (!scene_renderer_->Initialize(ctx.bootstrap->GetBackend(), total_vertex_count)) {
        return false;
    }

    // Create TechniqueManager and register standard technique
    technique_mgr_ = std::make_unique<VulkanEngine::TechniqueManager::TechniqueManager>();
    VulkanEngine::StandardMeshPipeline::PipelineConfig pipeline_config{};
    pipeline_config.cull_mode = vk::CullModeFlagBits::eFront;
    pipeline_config.front_face = vk::FrontFace::eClockwise;
    pipeline_config.depth_test_enable = true;
    pipeline_config.depth_write_enable = true;
    pipeline_config.depth_compare_op = vk::CompareOp::eLessOrEqual;
    const uint16_t main_technique = technique_mgr_->RegisterTechnique(
        *ctx.bootstrap, vert_spv, frag_spv, pipeline_config,
        bindless_mgr_->GetLayout(),
        scene_renderer_->GetInstanceDataLayout(),
        scene_renderer_->GetMainExpandedLayout());

    // Create MaterialManager
    material_mgr_ = std::make_unique<VulkanEngine::MaterialManager::MaterialManager>();
    material_mgr_->Initialize(*ctx.bootstrap, *technique_mgr_, *bindless_mgr_);

    VulkanEngine::MaterialManager::MaterialDefinition mat_def{};
    mat_def.technique_id = main_technique;
    mat_def.bindless_texture_slot = active_slot;
    mat_def.base_color_factor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    mat_def.metallic_factor = 0.0f;
    mat_def.roughness_factor = 1.0f;
    const uint16_t default_material = material_mgr_->RegisterMaterial(mat_def);

    // Upload combined scene
    const std::vector<uint16_t> material_ids(meshes.size(), default_material);
    combined_scene_ = VulkanEngine::SceneLoader::SceneManager::UploadCombined(
        *ctx.bootstrap, meshes, material_ids, default_material);

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

    const auto& backend = ctx.bootstrap->GetBackend();
    const auto surface_format = backend.GetSurfaceFormat();

    VulkanEngine::Backend::ImGui::ImGuiBackendConfig imgui_backend_config{};
    imgui_backend_config.image_count = ctx.bootstrap->GetSnapshot().swapchain_image_count;
    imgui_backend_config.swapchain_format = static_cast<vk::Format>(surface_format.format);

    VulkanEngine::ImGuiSystem::ImGuiSystemInitInfo imgui_init_info{};
    imgui_init_info.sdl_window = ctx.window;
    imgui_init_info.config.show_demo_window = false;
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
    auto& component_registry = ctx.bootstrap->GetBackend().GetComponentRegistry();
    auto& camera_entity = component_registry.CreateEntity();
    component_registry.AddComponent<VulkanEngine::Components::Camera>(camera_entity);

    // Create game entities
    for (size_t i = 0; i < combined_scene_.meshes.size(); ++i) {
        auto& entity = component_registry.CreateEntity();
        component_registry.AddComponent<VulkanEngine::Components::Transform>(entity);

        auto& mesh_ref = component_registry.AddComponent<VulkanEngine::Components::MeshReference>(entity);
        mesh_ref.vertex_offset = combined_scene_.meshes[i].vertex_offset;
        mesh_ref.vertex_count = combined_scene_.meshes[i].vertex_count;
        mesh_ref.index_offset = combined_scene_.meshes[i].index_offset;
        mesh_ref.index_count = combined_scene_.meshes[i].index_count;

        auto& material = component_registry.AddComponent<VulkanEngine::Components::Material>(entity);
        material.bindless_texture_slot = active_slot;
        material.technique_id = main_technique;

        if (i == 0) {
            component_registry.AddComponent<App::Components::DemoInputComponent>(entity, ctx.input_system);
        }
    }

    component_registry.InitializeAllComponents();

    camera_ = camera_entity.GetComponent<VulkanEngine::Components::Camera>();

    ctx.quit_action_handle = ctx.input_system->BindAction("quit", VulkanEngine::Input::InputBinding::Key(SDLK_ESCAPE));

    return true;
}

void DemoGame::OnPreInput(VulkanEngine::Application::ApplicationContext& ctx) {
}

bool DemoGame::ShouldFilterMouseInput() {
    return ImGui::GetIO().WantCaptureMouse;
}

bool DemoGame::ShouldFilterKeyboardInput() {
    return ImGui::GetIO().WantCaptureKeyboard;
}

void DemoGame::OnFrameUpdate(VulkanEngine::Application::ApplicationContext& ctx) {
    ctx.bootstrap->GetBackend().GetComponentRegistry().UpdateAllComponentsAsync(ctx.frame.delta_time);
}

void DemoGame::OnFrameRender(VulkanEngine::Application::ApplicationContext& ctx) {
    if (!renderer_ || !camera_ || !scene_valid_) {
        return;
    }

    renderer_->RenderFrame(*ctx.bootstrap,
                           ctx.bootstrap->GetBackend().GetComponentRegistry(),
                           *camera_,
                           combined_scene_.vertex_buffer,
                           combined_scene_.index_buffer,
                           *technique_mgr_,
                           *bindless_mgr_,
                           *scene_renderer_,
                           imgui_system_.get(),
                           ctx.frame.image_index);
}

void DemoGame::OnShutdown(VulkanEngine::Application::ApplicationContext& ctx) {
    if (renderer_) {
        renderer_->Shutdown();
        renderer_.reset();
    }
    if (scene_renderer_) {
        scene_renderer_->Shutdown();
        scene_renderer_.reset();
    }
    if (bindless_mgr_) {
        bindless_mgr_->Shutdown();
        bindless_mgr_.reset();
    }
    if (material_mgr_) {
        material_mgr_->Shutdown();
        material_mgr_.reset();
    }
    if (technique_mgr_) {
        technique_mgr_->Shutdown();
        technique_mgr_.reset();
    }
    if (scene_valid_) {
        combined_scene_.vertex_buffer = VulkanEngine::GpuResources::GpuBuffer{};
        combined_scene_.index_buffer = VulkanEngine::GpuResources::GpuBuffer{};
        combined_scene_.meshes.clear();
        scene_valid_ = false;
    }

    if (imgui_system_) {
        imgui_system_->Shutdown();
        imgui_system_.reset();
    }
}

} // namespace App::Game
