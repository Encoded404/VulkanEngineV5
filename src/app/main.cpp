#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <SDL3/SDL_keycode.h>
#include <imgui.h>
#include <logging/logging.hpp>
#include <vulkan/vulkan.hpp>

import VulkanEngine.Application;
import VulkanEngine.Components.MeshRenderer;
import VulkanEngine.Components.Transform;
import VulkanEngine.Input;
import VulkanBackend.RenderGraph;
import VulkanBackend.Utils.Timer;
import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;
import VulkanEngine.ImGuiSystem;
import VulkanEngine.RenderPipeline;
import VulkanEngine.ShaderLoader;
import VulkanEngine.DefaultResources;
import VulkanEngine.MeshRendererSystem;
import VulkanEngine.FrameRenderer;
import VulkanEngine.StandardMeshPipeline;
import App.DemoSceneRenderer;
import App.Components.DemoInputComponent;

namespace {
    struct DemoAppState {
        VulkanEngine::ResourceManager resource_manager{};
        std::shared_ptr<VulkanEngine::TextureResource> missing_texture{};
        VulkanEngine::ResourceHandle<VulkanEngine::TextureResource> fallback_handle{};
        std::filesystem::path exe_dir{};
        App::DemoSceneRenderer::MeshData mesh{};
        VulkanEngine::ResourceHandle<VulkanEngine::TextureResource> texture_handle{};
        VulkanEngine::TextureResource* active_texture = nullptr;
        std::vector<VulkanEngine::StandardMeshPipeline::Vertex> vertices{};
        std::vector<uint32_t> vert_spv{};
        std::vector<uint32_t> frag_spv{};
        bool scene_uploaded = false;
        std::shared_ptr<VulkanEngine::Backend::ImGui::IImGuiBackend> imgui_backend{};
        std::unique_ptr<VulkanEngine::ImGuiSystem::ImGuiSystem> imgui_system{};
        std::unique_ptr<VulkanEngine::RenderPipeline::RenderPipeline> pipeline{};
        std::unique_ptr<VulkanEngine::FrameRenderer::FrameRenderer> frame_renderer{};
        VulkanEngine::MeshRendererSystem::MeshRendererSystem mesh_renderer_system{};
        VulkanEngine::MeshRendererSystem::MeshRenderObject render_object{};
        std::unique_ptr<VulkanEngine::StandardMeshPipeline::PipelineManager> pipeline_manager{};
        App::DemoSceneRenderer::RenderPassData render_pass_data{};
        uint32_t swapchain_width = 0;
        uint32_t swapchain_height = 0;
    };
}

int main(int argc, char* const argv[]) {
    CLI::App app{"VulkanEngineV5 Demo"};

    std::string log_level_str = "info";
    app.add_option("-l,--log-level", log_level_str, "Console output log level (trace, debug, info, warn, error, critical)")
       ->check(CLI::IsMember({"trace", "debug", "info", "warn", "error", "critical"}));

    App::DemoSceneRenderer::RenderMode render_mode = App::DemoSceneRenderer::RenderMode::Normal;
    app.add_option("-m,--mode", render_mode, "Rendering mode")
       ->transform(CLI::CheckedTransformer(std::map<std::string, App::DemoSceneRenderer::RenderMode>{
           {"normal", App::DemoSceneRenderer::RenderMode::Normal},
           {"normals", App::DemoSceneRenderer::RenderMode::Normals},
           {"no-textures", App::DemoSceneRenderer::RenderMode::NoTextures}
       }, CLI::ignore_case));

    CLI11_PARSE(app, argc, argv);

    DemoAppState state{};
    const std::filesystem::path executable_path = std::filesystem::absolute(std::filesystem::path(argv[0]));

    VulkanEngine::Application::ApplicationConfig app_config{};
    app_config.app_name = "VulkanEngineV5 Demo";
    app_config.log_level = log_level_str;

    VulkanEngine::Application::ApplicationHooks hooks{};

    hooks.on_setup = [&](VulkanEngine::Application::ApplicationContext& ctx) -> bool {
        state.missing_texture = VulkanEngine::DefaultResources::DefaultResources::CreateCheckerboard(state.resource_manager);
        state.fallback_handle = VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>("checkerboard_default", &state.resource_manager);

        state.exe_dir = executable_path.parent_path();
        state.mesh = App::DemoSceneRenderer::DemoSceneManager::LoadMeshFromAssets(state.exe_dir / "models");
        state.texture_handle = App::DemoSceneRenderer::DemoSceneManager::LoadTexture(state.resource_manager, state.exe_dir / "textures", state.fallback_handle);
        state.active_texture = state.texture_handle.IsValid() ? state.texture_handle.Get() : state.missing_texture.get();

        auto demo_vertices = App::DemoSceneRenderer::DemoSceneManager::ConvertToDemoVertices(state.mesh);
        state.vertices.resize(demo_vertices.size());
        for (size_t i = 0; i < demo_vertices.size(); ++i) {
            state.vertices[i].px = demo_vertices[i].px;
            state.vertices[i].py = demo_vertices[i].py;
            state.vertices[i].pz = demo_vertices[i].pz;
            state.vertices[i].nx = demo_vertices[i].nx;
            state.vertices[i].ny = demo_vertices[i].ny;
            state.vertices[i].nz = demo_vertices[i].nz;
            state.vertices[i].u = demo_vertices[i].u;
            state.vertices[i].v = demo_vertices[i].v;
        }

        const std::filesystem::path shader_dir = SHADER_DIR;
        std::string frag_name = "textured.frag.spv";
        if (render_mode == App::DemoSceneRenderer::RenderMode::Normals) {
            frag_name = "normals.frag.spv";
        } else if (render_mode == App::DemoSceneRenderer::RenderMode::NoTextures) {
            frag_name = "solid.frag.spv";
        }

        state.vert_spv = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(shader_dir / "textured.vert.spv");
        state.frag_spv = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(shader_dir / frag_name);

        state.pipeline = std::make_unique<VulkanEngine::RenderPipeline::RenderPipeline>();
        state.pipeline->Initialize(*ctx.bootstrap);

        auto backbuffer = state.pipeline->ImportBackbuffer();
        auto depth_buffer = state.pipeline->ImportDepthBuffer();

        state.pipeline->SetInitialState(
            backbuffer,
            VulkanEngine::RenderGraph::ResourceState::ImageState(
                VulkanEngine::RenderGraph::PipelineStageIntent::TopOfPipe,
                VulkanEngine::RenderGraph::AccessIntent::None,
                VulkanEngine::RenderGraph::QueueType::Graphics,
                VulkanEngine::RenderGraph::ImageLayoutIntent::Undefined));

        (void)ctx.bootstrap->GetBackend().GetSwapchainExtent(state.swapchain_width, state.swapchain_height);

        App::DemoSceneRenderer::RenderPassData pass_data{};
        pass_data.bootstrap = ctx.bootstrap;
        pass_data.width = state.swapchain_width;
        pass_data.height = state.swapchain_height;

        VulkanEngine::RenderGraph::PassAttachmentSetup attachment_setup{};
        attachment_setup.auto_begin_rendering = true;

        VulkanEngine::RenderGraph::AttachmentInfo color_attach{};
        color_attach.resource = backbuffer;
        color_attach.load_op = vk::AttachmentLoadOp::eClear;
        color_attach.store_op = vk::AttachmentStoreOp::eStore;
        color_attach.clear_color = vk::ClearColorValue(std::array<float, 4>{0.1f, 0.1f, 0.1f, 1.0f});
        attachment_setup.color_attachments.push_back(color_attach);

        VulkanEngine::RenderGraph::AttachmentInfo depth_attach{};
        depth_attach.resource = depth_buffer;
        depth_attach.load_op = vk::AttachmentLoadOp::eClear;
        depth_attach.store_op = vk::AttachmentStoreOp::eDontCare;
        depth_attach.clear_depth = vk::ClearDepthStencilValue(1.0f, 0);
        attachment_setup.depth_attachment = depth_attach;

        VulkanEngine::RenderPipeline::RenderPipelinePassDesc render_pass_desc{};
        render_pass_desc.name = "demo-render";
        render_pass_desc.queue = VulkanEngine::RenderGraph::QueueType::Graphics;
        render_pass_desc.writes = {backbuffer, depth_buffer};
        render_pass_desc.attachments = attachment_setup;
        render_pass_desc.execute = [&state](const void* user_data, vk::CommandBuffer command_buffer) {
            const auto* pass_data = static_cast<const App::DemoSceneRenderer::RenderPassData*>(user_data);
            auto& registry = pass_data->bootstrap->GetBackend().GetComponentRegistry();
            state.mesh_renderer_system.RecordAllMeshDraws(command_buffer, registry, state.render_object, pass_data->width, pass_data->height);
        };

        state.pipeline->AddPass(render_pass_desc);
        state.pipeline->Compile();
        if (!state.pipeline->IsCompiled()) {
            return false;
        }

        state.pipeline_manager = std::make_unique<VulkanEngine::StandardMeshPipeline::PipelineManager>();
        state.pipeline_manager->Initialize(*ctx.bootstrap, state.vert_spv, state.frag_spv);

        if (!App::DemoSceneRenderer::DemoSceneManager::UploadDemoScene(
                *ctx.bootstrap,
                reinterpret_cast<const VulkanEngine::StandardMeshPipeline::Vertex*>(state.vertices.data()),
                static_cast<uint32_t>(state.vertices.size()),
                state.mesh.indices.data(),
                static_cast<uint32_t>(state.mesh.indices.size()),
                state.active_texture,
                state.pipeline_manager.get())) {
            return false;
        }
        state.scene_uploaded = true;
        state.render_object = App::DemoSceneRenderer::DemoSceneManager::GetMeshRenderObject();

        state.imgui_backend = VulkanEngine::Backend::ImGui::CreateImGuiBackend();
        state.imgui_system = std::make_unique<VulkanEngine::ImGuiSystem::ImGuiSystem>(state.imgui_backend);

        const auto& backend = ctx.bootstrap->GetBackend();
        const auto surface_format = backend.GetSurfaceFormat();

        VulkanEngine::Backend::ImGui::ImGuiBackendConfig imgui_backend_config{};
        imgui_backend_config.image_count = ctx.bootstrap->GetSnapshot().swapchain_image_count;
        imgui_backend_config.swapchain_format = static_cast<vk::Format>(surface_format.format);

        VulkanEngine::ImGuiSystem::ImGuiSystemInitInfo imgui_init_info{};
        imgui_init_info.sdl_window = ctx.window;
        imgui_init_info.config.show_demo_window = true;
        imgui_init_info.backend_config = imgui_backend_config;
        imgui_init_info.instance = backend.GetInstance();
        imgui_init_info.physical_device = backend.GetPhysicalDevice();
        imgui_init_info.device = backend.GetDevice();
        imgui_init_info.queue_family = backend.GetGraphicsQueueFamily();
        imgui_init_info.queue = backend.GetGraphicsQueue();
        imgui_init_info.api_version = VK_API_VERSION_1_3;

        if (!state.imgui_system->Initialize(imgui_init_info)) {
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
            state.imgui_system.get());

        auto& component_registry = ctx.bootstrap->GetBackend().GetComponentRegistry();
        auto& demo_entity = component_registry.CreateEntity();
        component_registry.AddComponent<VulkanEngine::Components::Transform>(demo_entity);
        component_registry.AddComponent<VulkanEngine::Components::MeshRenderer>(demo_entity);
        component_registry.AddComponent<App::Components::DemoInputComponent>(demo_entity, ctx.input_system);
        component_registry.InitializeAllComponents();

        ctx.quit_action_handle = ctx.input_system->BindAction("quit", VulkanEngine::Input::InputBinding::Key(SDLK_ESCAPE));

        VulkanEngine::FrameRenderer::FrameRendererConfig frame_config{};
        frame_config.enable_imgui = true;
        state.frame_renderer = std::make_unique<VulkanEngine::FrameRenderer::FrameRenderer>();
        state.frame_renderer->Initialize(*ctx.bootstrap, *state.pipeline, frame_config);

        return true;
    };

    hooks.on_pre_input = [&]([[maybe_unused]] VulkanEngine::Application::ApplicationContext& ctx) {
    };

    hooks.should_filter_mouse_input = []() -> bool {
        return ImGui::GetIO().WantCaptureMouse;
    };

    hooks.should_filter_keyboard_input = []() -> bool {
        return ImGui::GetIO().WantCaptureKeyboard;
    };

    hooks.on_frame_update = [&](VulkanEngine::Application::ApplicationContext& ctx) {
        ctx.bootstrap->GetBackend().GetComponentRegistry().UpdateAllComponentsAsync(ctx.frame.delta_time);
    };

    hooks.on_frame_render = [&](VulkanEngine::Application::ApplicationContext& ctx) {
        if (!state.pipeline || !state.pipeline->IsCompiled()) {
            return;
        }

        state.render_pass_data.bootstrap = ctx.bootstrap;
        state.render_pass_data.image_index = ctx.frame.image_index;
        (void)ctx.bootstrap->GetBackend().GetSwapchainExtent(state.render_pass_data.width, state.render_pass_data.height);

        state.frame_renderer->RenderFrame(*state.pipeline, &state.render_pass_data,
                                          state.imgui_system.get(), ctx.frame.image_index);
    };

    hooks.on_shutdown = [&]([[maybe_unused]] VulkanEngine::Application::ApplicationContext& ctx) {
        VulkanEngine::Utils::Timer const t{true};
        double prev = 0.0;
        if (state.frame_renderer) {
            state.frame_renderer->Shutdown();
            state.frame_renderer.reset();
            const double current = t.ElapsedMs();
            LOGIFACE_LOG(info, "shutdown: frame_renderer " + std::to_string(current - prev) + " ms");
            prev = current;
        }
        if (state.pipeline) {
            state.pipeline->Shutdown();
            state.pipeline.reset();
            const double current = t.ElapsedMs();
            LOGIFACE_LOG(info, "shutdown: render_pipeline " + std::to_string(current - prev) + " ms");
            prev = current;
        }
        if (state.scene_uploaded && ctx.bootstrap != nullptr) {
            App::DemoSceneRenderer::DemoSceneManager::DestroyDemoSceneResources(*ctx.bootstrap);
            state.scene_uploaded = false;
            const double current = t.ElapsedMs();
            LOGIFACE_LOG(info, "shutdown: demo_scene " + std::to_string(current - prev) + " ms");
            prev = current;
        }
        if (state.pipeline_manager) {
            state.pipeline_manager->Shutdown();
            state.pipeline_manager.reset();
            const double current = t.ElapsedMs();
            LOGIFACE_LOG(info, "shutdown: pipeline_manager " + std::to_string(current - prev) + " ms");
            prev = current;
        }
        if (state.imgui_system) {
            state.imgui_system->Shutdown();
            state.imgui_system.reset();
            const double current = t.ElapsedMs();
            LOGIFACE_LOG(info, "shutdown: imgui " + std::to_string(current - prev) + " ms");
            prev = current;
        }
    };

    return VulkanEngine::Application::RunApplication(app_config, hooks);
}
