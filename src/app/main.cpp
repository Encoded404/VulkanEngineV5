#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <iostream>
#include <map>

#include <SDL3/SDL_video.h>
#include <boost/stacktrace.hpp>
#include <CLI/CLI.hpp>

#include <logging/logging.hpp>
#include <logging/ConsoleLogger.hpp>

import VulkanEngine.Platform.SdlPlatformShell;
import VulkanEngine.Platform.SdlPlatformBackend;
import VulkanEngine.Runtime.RuntimeShell;
import VulkanEngine.Runtime.VulkanBootstrap;
import VulkanEngine.Runtime.VulkanBootstrapBackend;
import VulkanEngine.RenderGraph;
import VulkanEngine.RenderGraph.GraphExecutionBridge;
import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;
import App.DemoScene;

namespace {
    void InitializeLogger(Logiface::Level level) {
        static std::shared_ptr<Logiface::ConsoleLogger> app_logger = std::make_shared<Logiface::ConsoleLogger>(); // NOLINT(misc-const-correctness)
        app_logger->SetLevel(level);
        Logiface::SetLogger(app_logger);
    }

    Logiface::Level ParseLogLevel(const std::string& level) {
        if (level == "trace") return Logiface::Level::trace;
        if (level == "debug") return Logiface::Level::debug;
        if (level == "info") return Logiface::Level::info;
        if (level == "warn") return Logiface::Level::warn;
        if (level == "error") return Logiface::Level::error;
        if (level == "critical") return Logiface::Level::critical;
        return Logiface::Level::info;
    }
} // anonymous namespace

int main(int argc, char** argv) {
    CLI::App app{"VulkanEngineV5 Demo"};

    std::string log_level_str = "info";
    app.add_option("-l,--log-level", log_level_str, "Console output log level (trace, debug, info, warn, error, critical)")
       ->check(CLI::IsMember({"trace", "debug", "info", "warn", "error", "critical"}));

    App::DemoScene::RenderMode render_mode = App::DemoScene::RenderMode::Normal;
    app.add_option("-m,--mode", render_mode, "Rendering mode")
       ->transform(CLI::CheckedTransformer(std::map<std::string, App::DemoScene::RenderMode>{
           {"normal", App::DemoScene::RenderMode::Normal},
           {"normals", App::DemoScene::RenderMode::Normals},
           {"no-textures", App::DemoScene::RenderMode::NoTextures}
       }, CLI::ignore_case));

    CLI11_PARSE(app, argc, argv);

    bool platform_initialized = false;
    bool bootstrap_initialized = false;
    bool runtime_initialized = false;
    bool scene_uploaded = false;
    std::shared_ptr<VulkanEngine::Platform::IPlatformBackend> platform_backend{};
    std::unique_ptr<VulkanEngine::Platform::SdlPlatformShell> platform{};
    std::shared_ptr<VulkanEngine::Runtime::IVulkanBootstrapBackend> vk_backend{};
    std::unique_ptr<VulkanEngine::Runtime::VulkanBootstrap> bootstrap{};
    std::unique_ptr<VulkanEngine::Runtime::RuntimeShell> runtime{};

    try {
        InitializeLogger(ParseLogLevel(log_level_str));
        LOGIFACE_LOG(info, "VulkanEngineV5 demo started");

        platform_backend = VulkanEngine::Platform::CreateSdlPlatformBackend();
        platform = std::make_unique<VulkanEngine::Platform::SdlPlatformShell>(platform_backend);
        if (!platform->Initialize(VulkanEngine::Platform::PlatformConfig{})) {
            throw std::runtime_error("Platform initialization failed");
        }
        platform_initialized = true;

        SDL_Window* window = platform->GetNativeWindowHandle();
        if (window == nullptr) {
            throw std::runtime_error("Native SDL window handle is null");
        }

        VulkanEngine::ResourceManager resource_manager;

        // Create the global missing texture
        VulkanEngine::CheckerboardConfig missing_texture_config;
        missing_texture_config.color1 = {255, 255, 255, 255}; // White
        missing_texture_config.color2 = {255, 0, 255, 255};   // Purple

        // Use an internal registration mechanism or just a shared pointer if ResourceHandle doesn't support manual injection easily.
        // For now, let's assume we can create it and keep a handle to it.
        auto missing_texture = VulkanEngine::TextureResource::CreateCheckerboardTexture("missing_texture", missing_texture_config);
        const VulkanEngine::ResourceHandle<VulkanEngine::TextureResource> fallback_handle("missing_texture", &resource_manager);

        const std::filesystem::path exe_dir = std::filesystem::absolute(std::filesystem::path(argv[0])).parent_path();
        const auto mesh = App::DemoScene::DemoSceneManager::LoadMeshFromAssets(exe_dir / "models");

        auto texture_handle = App::DemoScene::DemoSceneManager::LoadTexture(resource_manager, exe_dir / "textures", fallback_handle);

        // If the handle is the fallback, we need to make sure the resource exists in the manager or use the pointer.
        VulkanEngine::TextureResource* active_texture = texture_handle.IsValid() ? texture_handle.Get() : missing_texture.get();

        const auto demo_vertices = App::DemoScene::DemoSceneManager::ConvertToDemoVertices(mesh);

        const std::filesystem::path shader_dir = SHADER_DIR;
        std::string const vert_name = "textured.vert.spv";
        std::string frag_name = "textured.frag.spv";

        if (render_mode == App::DemoScene::RenderMode::Normals) {
            frag_name = "normals.frag.spv";
        } else if (render_mode == App::DemoScene::RenderMode::NoTextures) {
            frag_name = "solid.frag.spv";
        }

        const auto vert_spv = App::DemoScene::DemoSceneManager::ReadSpirv(shader_dir / vert_name);
        const auto frag_spv = App::DemoScene::DemoSceneManager::ReadSpirv(shader_dir / frag_name);

        vk_backend = VulkanEngine::Runtime::CreateVulkanBootstrapBackend();
        bootstrap = std::make_unique<VulkanEngine::Runtime::VulkanBootstrap>(vk_backend);
        VulkanEngine::Runtime::VulkanBootstrapConfig vk_config{};
        vk_config.native_window_handle = window;
        if (!bootstrap->Initialize(vk_config)) {
            throw std::runtime_error("Vulkan bootstrap initialization failed");
        }
        bootstrap_initialized = true;

        if (!App::DemoScene::DemoSceneManager::UploadDemoScene(
                *bootstrap,
                demo_vertices.data(),
                static_cast<uint32_t>(demo_vertices.size()),
                mesh.indices.data(),
                static_cast<uint32_t>(mesh.indices.size()),
                active_texture,
                vert_spv.data(),
                vert_spv.size() * sizeof(uint32_t),
                frag_spv.data(),
                frag_spv.size() * sizeof(uint32_t)
                )
            )
        {
            throw std::runtime_error("Failed to upload demo scene to Vulkan backend");
        }
        scene_uploaded = true;

        VulkanEngine::RenderGraph::RenderGraphBuilder render_graph{};
        const auto backbuffer = render_graph.ImportResource("swapchain-backbuffer", VulkanEngine::RenderGraph::ResourceKind::Image);
        render_graph.SetFinalState(
                backbuffer,
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
                const auto* frame = static_cast<const App::DemoScene::FrameRenderData*>(user_data);
                if (frame != nullptr && frame->bootstrap != nullptr) {
                    auto* mutable_frame = const_cast<App::DemoScene::FrameRenderData*>(frame);
                    mutable_frame->render_success = App::DemoScene::DemoSceneManager::RenderDemoFrame(*frame->bootstrap, mutable_frame->image_index, mutable_frame->angle_degrees);
                }
            });

        render_graph.AddWrite(render_pass, backbuffer);

        const auto compiled_graph = render_graph.Compile();
        if (!compiled_graph.success) {
            throw std::runtime_error("Render graph compilation failed");
        }

        runtime = std::make_unique<VulkanEngine::Runtime::RuntimeShell>();
        if (!runtime->Initialize(VulkanEngine::Runtime::RuntimeConfig{})) {
            throw std::runtime_error("Runtime shell initialization failed");
        }
        runtime_initialized = true;

        auto previous_time = std::chrono::steady_clock::now();
        float angle = 0.0f;

        while (!platform->ShouldQuit() && !runtime->ShouldShutdown()) {
            LOGIFACE_LOG(trace, "new frame started");
            platform->PollEvents();
            const auto& platform_state = platform->GetState();

            if (platform_state.quit_requested) {
                runtime->RequestShutdown();
            }

            runtime->NotifyWindowMinimized(platform_state.minimized);
            const auto runtime_frame = runtime->BeginFrame();
            if (runtime_frame.status == VulkanEngine::Runtime::RuntimeStatus::Minimized ||
                runtime_frame.status == VulkanEngine::Runtime::RuntimeStatus::ShutdownRequested) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (platform_state.resized) {
                bootstrap->NotifySwapchainOutOfDate();
            }

            const auto bootstrap_frame = bootstrap->BeginFrame();
            if (bootstrap_frame.status == VulkanEngine::Runtime::BootstrapStatus::SwapchainOutOfDate) {
                if (!bootstrap->RecreateSwapchain()) {
                    throw std::runtime_error("Swapchain recreation failed");
                }
                continue;
            }
            if (bootstrap_frame.status != VulkanEngine::Runtime::BootstrapStatus::Ok) {
                throw std::runtime_error("Vulkan bootstrap entered non-OK frame status");
            }

            uint32_t image_index = 0;
            if (!bootstrap->AcquireNextImage(image_index)) {
                bootstrap->NotifySwapchainOutOfDate();
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - previous_time).count();
            previous_time = now;
            angle += dt * 90.0f;
            if (angle >= 360.0f) {
                angle -= 360.0f;
            }

            const VulkanEngine::RenderGraph::GraphExecutionContext graph_context = VulkanEngine::RenderGraph::CreateGraphExecutionContext(
                runtime_frame,
                VulkanEngine::RenderGraph::ImportedFrameResources{.backbuffer = backbuffer});

            App::DemoScene::FrameRenderData frame_render_data{
                .bootstrap = bootstrap.get(),
                .graph_context = &graph_context,
                .angle_degrees = angle,
                .image_index = image_index,
                .render_success = true,
            };

            compiled_graph.Execute(&frame_render_data);

            // Always attempt to Present if an image was acquired.
            // If rendering failed, the backend will submit a dummy sync to clean up semaphores.
            if (!bootstrap->Present(image_index, frame_render_data.render_success)) {
                bootstrap->NotifySwapchainOutOfDate();
            }

            runtime->EndFrame();
            bootstrap->EndFrame();
        }

        if (scene_uploaded) {
            App::DemoScene::DemoSceneManager::DestroyDemoSceneResources(*bootstrap);
            scene_uploaded = false;
        }
        if (runtime_initialized) {
            runtime->Shutdown();
            runtime_initialized = false;
        }
        if (bootstrap_initialized) {
            bootstrap->Shutdown();
            bootstrap_initialized = false;
        }
        if (platform_initialized) {
            platform->Shutdown();
            platform_initialized = false;
        }

        LOGIFACE_LOG(info, "App completed");
        return 0;
    } catch (const std::exception& ex) {
        auto trace = boost::stacktrace::stacktrace::from_current_exception();
        InitializeLogger(Logiface::Level::info);
        LOGIFACE_LOG(error, std::string("Fatal error: ") + ex.what());
        std::cerr << "\nStacktrace:\n" << trace << '\n';
        if (scene_uploaded && bootstrap_initialized && bootstrap) {
            App::DemoScene::DemoSceneManager::DestroyDemoSceneResources(*bootstrap);
            scene_uploaded = false;
        }
        if (runtime_initialized && runtime) {
            runtime->Shutdown();
        }
        if (bootstrap_initialized && bootstrap) {
            bootstrap->Shutdown();
        }
        if (platform_initialized && platform) {
            platform->Shutdown();
        }
        return 1;
    }
}
