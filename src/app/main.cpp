#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

import VulkanEngine.Application;
import VulkanEngine.Components.MeshRenderer;
import VulkanEngine.Components.Transform;
import VulkanEngine.RenderGraph;
import VulkanEngine.RenderGraph.GraphExecutionBridge;
import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;
import App.DemoSceneRenderer;
import App.Components.DemoInputComponent;

namespace {
    struct DemoAppState {
        VulkanEngine::ResourceManager resource_manager{};
        VulkanEngine::CheckerboardConfig missing_texture_config{};
        std::shared_ptr<VulkanEngine::TextureResource> missing_texture{};
        VulkanEngine::ResourceHandle<VulkanEngine::TextureResource> fallback_handle{};
        std::filesystem::path exe_dir{};
        App::DemoSceneRenderer::MeshData mesh{};
        VulkanEngine::ResourceHandle<VulkanEngine::TextureResource> texture_handle{};
        VulkanEngine::TextureResource* active_texture = nullptr;
        std::vector<App::DemoSceneRenderer::DemoVertex> demo_vertices{};
        std::vector<uint32_t> vert_spv{};
        std::vector<uint32_t> frag_spv{};
        VulkanEngine::RenderGraph::ResourceHandle backbuffer{};
        VulkanEngine::RenderGraph::CompileResult compiled_graph{};
        bool scene_uploaded = false;
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
        state.missing_texture_config.color1 = {255, 255, 255, 255};
        state.missing_texture_config.color2 = {255, 0, 255, 255};

        state.missing_texture = VulkanEngine::TextureResource::CreateCheckerboardTexture("missing_texture", state.missing_texture_config);
        state.fallback_handle = VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>("missing_texture", &state.resource_manager);

        state.exe_dir = executable_path.parent_path();
        state.mesh = App::DemoSceneRenderer::DemoSceneManager::LoadMeshFromAssets(state.exe_dir / "models");
        state.texture_handle = App::DemoSceneRenderer::DemoSceneManager::LoadTexture(state.resource_manager, state.exe_dir / "textures", state.fallback_handle);
        state.active_texture = state.texture_handle.IsValid() ? state.texture_handle.Get() : state.missing_texture.get();
        state.demo_vertices = App::DemoSceneRenderer::DemoSceneManager::ConvertToDemoVertices(state.mesh);

        const std::filesystem::path shader_dir = SHADER_DIR;
        const std::string vert_name = "textured.vert.spv";
        std::string frag_name = "textured.frag.spv";
        if (render_mode == App::DemoSceneRenderer::RenderMode::Normals) {
            frag_name = "normals.frag.spv";
        } else if (render_mode == App::DemoSceneRenderer::RenderMode::NoTextures) {
            frag_name = "solid.frag.spv";
        }

        state.vert_spv = App::DemoSceneRenderer::DemoSceneManager::ReadSpirv(shader_dir / vert_name);
        state.frag_spv = App::DemoSceneRenderer::DemoSceneManager::ReadSpirv(shader_dir / frag_name);

        VulkanEngine::RenderGraph::RenderGraphBuilder render_graph{};
        state.backbuffer = render_graph.ImportResource("swapchain-backbuffer", VulkanEngine::RenderGraph::ResourceKind::Image);
        render_graph.SetFinalState(
            state.backbuffer,
            VulkanEngine::RenderGraph::ResourceState::ImageState(
                VulkanEngine::RenderGraph::PipelineStageIntent::Present,
                VulkanEngine::RenderGraph::AccessIntent::Read,
                VulkanEngine::RenderGraph::QueueType::Graphics,
                VulkanEngine::RenderGraph::ImageLayoutIntent::Present));

        const auto render_pass = render_graph.AddPass(
            "demo-render",
            VulkanEngine::RenderGraph::QueueType::Graphics,
            true,
            [](const void* user_data) {
                const auto* frame = static_cast<const App::DemoSceneRenderer::FrameRenderData*>(user_data);
                if (frame != nullptr && frame->bootstrap != nullptr) {
                    auto* mutable_frame = const_cast<App::DemoSceneRenderer::FrameRenderData*>(frame);
                    mutable_frame->render_success = App::DemoSceneRenderer::DemoSceneManager::RenderDemoFrame(
                        *frame->bootstrap,
                        mutable_frame->image_index);
                }
            });

        render_graph.AddWrite(render_pass, state.backbuffer);
        state.compiled_graph = render_graph.Compile();
        if (!state.compiled_graph.success) {
            return false;
        }

        if (!App::DemoSceneRenderer::DemoSceneManager::UploadDemoScene(
                *ctx.bootstrap,
                state.demo_vertices.data(),
                static_cast<uint32_t>(state.demo_vertices.size()),
                state.mesh.indices.data(),
                static_cast<uint32_t>(state.mesh.indices.size()),
                state.active_texture,
                state.vert_spv.data(),
                state.vert_spv.size() * sizeof(uint32_t),
                state.frag_spv.data(),
                state.frag_spv.size() * sizeof(uint32_t))) {
            return false;
        }
        state.scene_uploaded = true;

        auto& component_registry = ctx.bootstrap->GetBackend().GetComponentRegistry();
        auto& demo_entity = component_registry.CreateEntity();
        component_registry.AddComponent<App::Components::Transform>(demo_entity);
        component_registry.AddComponent<App::Components::MeshRenderer>(demo_entity);
        component_registry.AddComponent<App::Components::DemoInputComponent>(demo_entity, ctx.input_system);
        component_registry.InitializeAllComponents();
        return true;
    };

    hooks.on_frame_update = [&](VulkanEngine::Application::ApplicationContext& ctx) {
        ctx.bootstrap->GetBackend().GetComponentRegistry().UpdateAllComponentsAsync(ctx.frame.delta_time);
    };

    hooks.on_frame_render = [&](VulkanEngine::Application::ApplicationContext& ctx) {
        const VulkanEngine::RenderGraph::GraphExecutionContext graph_context = VulkanEngine::RenderGraph::CreateGraphExecutionContext(
            ctx.frame.runtime_frame,
            VulkanEngine::RenderGraph::ImportedFrameResources{.backbuffer = state.backbuffer});

        App::DemoSceneRenderer::FrameRenderData frame_render_data{
            .bootstrap = ctx.bootstrap,
            .graph_context = &graph_context,
            .image_index = ctx.frame.image_index,
            .render_success = true,
        };

        state.compiled_graph.Execute(&frame_render_data);
        ctx.frame.render_success = frame_render_data.render_success;
    };

    hooks.on_shutdown = [&](VulkanEngine::Application::ApplicationContext& ctx) {
        if (state.scene_uploaded && ctx.bootstrap != nullptr) {
            App::DemoSceneRenderer::DemoSceneManager::DestroyDemoSceneResources(*ctx.bootstrap);
            state.scene_uploaded = false;
        }
    };

    return VulkanEngine::Application::RunApplication(app_config, hooks);
}
