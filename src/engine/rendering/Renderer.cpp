module;

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/matrix_transform.hpp> //NOLINT(misc-include-cleaner)

#include <logging/logging_macros.hpp>

module VulkanEngine.Renderer;

import std;
import std.compat;

import logiface;

import vulkan_hpp;

import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanBackend.Utils.VulkanDebugUtils;
import VulkanBackend.RenderGraph;
import VulkanEngine.RenderPipeline;
import VulkanEngine.SceneRenderer;
import VulkanEngine.TechniqueManager;
import VulkanEngine.BindlessManager;
import VulkanEngine.Components.Camera;
import VulkanEngine.GpuResources;
import VulkanEngine.ImGui;

namespace VulkanEngine::Renderer {

Renderer::~Renderer() {
    Shutdown();
}

bool Renderer::Initialize(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                  const RendererConfig& config) {
    bootstrap_ = &bootstrap;

    pipeline_ = std::make_unique<VulkanEngine::RenderPipeline::RenderPipeline>();
    pipeline_->Initialize(bootstrap);

    auto backbuffer = pipeline_->ImportBackbuffer();
    auto depth_buffer = pipeline_->ImportDepthBuffer();
    auto hiz_image = pipeline_->ImportImage("hiz-image");
    auto scene_buffers = pipeline_->ImportBuffer("scene-buffers");
    auto draw_indirect = pipeline_->ImportBuffer("draw-indirect");

    pipeline_->RegisterResourceResolver("hiz-image",
        [this](std::uint32_t) { return current_scene_renderer_ ? current_scene_renderer_->GetHizImage(frame_counter_) : nullptr; },
        [this](std::uint32_t) { return current_scene_renderer_ ? current_scene_renderer_->GetHizFullView(frame_counter_) : nullptr; },
        vk::Format::eR32Sfloat);

    pipeline_->SetFinalState(
        backbuffer,
        VulkanEngine::RenderGraph::ResourceState::ImageState(
            VulkanEngine::RenderGraph::PipelineStageIntent::BottomOfPipe,
            VulkanEngine::RenderGraph::AccessIntent::None,
            VulkanEngine::RenderGraph::QueueType::Graphics,
            VulkanEngine::RenderGraph::ImageLayoutIntent::Present));

    // ── Pass 1: Expand (compute) ──
    auto expand_pass = pipeline_->AddPass({
        .name = "expand",
        .queue = VulkanEngine::RenderGraph::QueueType::Graphics,
        .writes = {scene_buffers, draw_indirect},
        .execute = [this](const void*, vk::CommandBuffer cmd) {
            if (current_scene_renderer_) {
                const std::uint32_t cnt = current_scene_renderer_->GetCurrentEntityCount();
                if (cnt) {
                    current_scene_renderer_->DispatchExpand(cmd, cnt,
                        current_view_proj_, frame_counter_);
                }
            }
        }
    });

    // ── Pass 2: Depth pre-pass ──
    VulkanEngine::RenderGraph::PassAttachmentSetup depth_setup{};
    depth_setup.auto_begin_rendering = true;

    VulkanEngine::RenderGraph::AttachmentInfo depth_attach{};
    depth_attach.resource = depth_buffer;
    depth_attach.load_op = vk::AttachmentLoadOp::eClear;
    depth_attach.store_op = vk::AttachmentStoreOp::eStore;
    depth_attach.clear_depth = config.clear_depth_stencil;
    depth_setup.depth_attachment = depth_attach;

    auto depth_pass = pipeline_->AddPass({
        .name = "depth-prepass",
        .queue = VulkanEngine::RenderGraph::QueueType::Graphics,
        .reads = {{scene_buffers,
            VulkanEngine::RenderGraph::PipelineStageIntent::VertexShader,
            VulkanEngine::RenderGraph::AccessIntent::Read},
            {draw_indirect,
            VulkanEngine::RenderGraph::PipelineStageIntent::IndirectDraw,
            VulkanEngine::RenderGraph::AccessIntent::Read}},
        .writes = {depth_buffer},
        .attachments = depth_setup,
        .execute = [this](const void*, vk::CommandBuffer cmd) {
            if (current_scene_renderer_) {
                current_scene_renderer_->DepthPrepass(cmd, current_width_, current_height_,
                                                     frame_counter_);
            }
        }
    });

    // ── Pass 3: Hi-Z generation compute ──
    auto hiz_pass = pipeline_->AddPass({
        .name = "hiz-gen",
        .queue = VulkanEngine::RenderGraph::QueueType::Graphics,
        .reads = {{depth_buffer,
            VulkanEngine::RenderGraph::PipelineStageIntent::ComputeShader,
            VulkanEngine::RenderGraph::AccessIntent::Read}},
        .writes = {hiz_image},
        .execute = [this](const void*, vk::CommandBuffer cmd) {
            if (current_scene_renderer_) {
                current_scene_renderer_->DispatchHiZGen(cmd, current_width_, current_height_,
                                                        frame_counter_, current_image_index_);
            }
        }
    });

    // ── Pass 4: Occlusion cull compute ──
    auto occlusion_pass = pipeline_->AddPass({
        .name = "occlusion",
        .queue = VulkanEngine::RenderGraph::QueueType::Graphics,
        .reads = {{hiz_image,
            VulkanEngine::RenderGraph::PipelineStageIntent::ComputeShader,
            VulkanEngine::RenderGraph::AccessIntent::Read},
            {scene_buffers,
            VulkanEngine::RenderGraph::PipelineStageIntent::ComputeShader,
            VulkanEngine::RenderGraph::AccessIntent::Read}},
        .writes = {scene_buffers},
        .execute = [this](const void*, vk::CommandBuffer cmd) {
            if (current_scene_renderer_) {
                current_scene_renderer_->DispatchOcclusion(cmd, frame_counter_);
            }
        }
    });

    // ── Pass 5: Collect compute (count + compact + draw commands) ──
    auto collect_pass = pipeline_->AddPass({
        .name = "collect",
        .queue = VulkanEngine::RenderGraph::QueueType::Graphics,
        .reads = {{scene_buffers,
            VulkanEngine::RenderGraph::PipelineStageIntent::ComputeShader,
            VulkanEngine::RenderGraph::AccessIntent::Read}},
        .writes = {scene_buffers, draw_indirect},
        .execute = [this](const void*, vk::CommandBuffer cmd) {
            if (current_scene_renderer_) {
                current_scene_renderer_->DispatchCollect(cmd, frame_counter_);
            }
        }
    });

    // ── Pass 6: Main pass (opaque) ──
    VulkanEngine::RenderGraph::PassAttachmentSetup main_setup{};
    main_setup.auto_begin_rendering = true;

    VulkanEngine::RenderGraph::AttachmentInfo color_attach{};
    color_attach.resource = backbuffer;
    color_attach.load_op = vk::AttachmentLoadOp::eClear;
    color_attach.store_op = vk::AttachmentStoreOp::eStore;
    color_attach.clear_color = vk::ClearColorValue(std::array<float, 4>{
        config.clear_color.r, config.clear_color.g, config.clear_color.b, config.clear_color.a});
    main_setup.color_attachments.push_back(color_attach);

    VulkanEngine::RenderGraph::AttachmentInfo main_depth_attach{};
    main_depth_attach.resource = depth_buffer;
    main_depth_attach.load_op = vk::AttachmentLoadOp::eLoad;
    main_depth_attach.store_op = vk::AttachmentStoreOp::eStore;
    main_setup.depth_attachment = main_depth_attach;

    auto main_pass = pipeline_->AddPass({
        .name = "main-pass",
        .queue = VulkanEngine::RenderGraph::QueueType::Graphics,
        .reads = {{scene_buffers,
            VulkanEngine::RenderGraph::PipelineStageIntent::VertexShader,
            VulkanEngine::RenderGraph::AccessIntent::Read},
            {draw_indirect,
            VulkanEngine::RenderGraph::PipelineStageIntent::IndirectDraw,
            VulkanEngine::RenderGraph::AccessIntent::Read}},
        .writes = {backbuffer, depth_buffer},
        .attachments = main_setup,
        .execute = [this](const void*, vk::CommandBuffer cmd) {
            if (current_scene_renderer_) {
                const float aspect = static_cast<float>(current_width_) / static_cast<float>(current_height_);
                const glm::mat4 view = current_camera_->GetViewMatrix();
                const glm::mat4 proj = current_camera_->GetProjectionMatrix(aspect);

                current_scene_renderer_->Render(cmd, *current_registry_,
                                                *current_technique_mgr_, *current_bindless_mgr_,
                                                proj, view,
                                                current_width_, current_height_,
                                                frame_counter_);
            }
        }
    });

    // ── Pass 7: ImGui overlay ──
    if (config.enable_imgui) {
        VulkanEngine::RenderGraph::PassAttachmentSetup imgui_setup{};
        imgui_setup.auto_begin_rendering = false;

        VulkanEngine::RenderGraph::AttachmentInfo imgui_color_attach{};
        imgui_color_attach.resource = backbuffer;
        imgui_color_attach.load_op = vk::AttachmentLoadOp::eLoad;
        imgui_color_attach.store_op = vk::AttachmentStoreOp::eStore;
        imgui_setup.color_attachments.push_back(imgui_color_attach);

        pipeline_->AddPass({
            .name = "imgui-overlay",
            .queue = VulkanEngine::RenderGraph::QueueType::Graphics,
            .writes = {backbuffer},
            .attachments = imgui_setup,
            .execute = [this](const void*, vk::CommandBuffer cmd) {
                if (current_imgui_ && current_imgui_->IsInitialized()) {
                    auto& backend = bootstrap_->GetBackend();
                    current_imgui_->RenderDrawData(cmd,
                        *backend.GetSwapchainImageViews()[current_image_index_],
                        current_width_, current_height_);
                }
            }
        });
    }

    // Explicit ordering ensures correct pipeline
    pipeline_->AddDependency(expand_pass, depth_pass);
    pipeline_->AddDependency(depth_pass, hiz_pass);
    pipeline_->AddDependency(hiz_pass, occlusion_pass);
    pipeline_->AddDependency(occlusion_pass, collect_pass);
    pipeline_->AddDependency(collect_pass, main_pass);

    pipeline_->Compile();
    if (!pipeline_->IsCompiled()) return false;

    clear_depth_stencil_ = config.clear_depth_stencil;

    {
        auto& device = bootstrap.GetBackend().GetDevice();
        vk::QueryPoolCreateInfo qp_info{};
        qp_info.queryType = vk::QueryType::ePipelineStatistics;
        qp_info.pipelineStatistics = GPU_STATS_FLAGS;
        qp_info.queryCount = 1;
        gpu_stats_pool_ = std::make_unique<vk::raii::QueryPool>(device, qp_info);
        VulkanEngine::Utils::SetVulkanObjectName(device, *gpu_stats_pool_, "gpu-stats-pool");
        const vk::Device raw_device = *device;
        raw_device.resetQueryPool(*gpu_stats_pool_, 0, 1);
    }

    LOGIFACE_LOG(info, "Renderer initialized with full render-graph pipeline");
    return true;
}

void Renderer::Shutdown() {
    if (bootstrap_) {
        try {
            bootstrap_->GetBackend().GetDevice().waitIdle();
        } catch (const std::exception& err) {
            LOGIFACE_LOG(error, "Error during Renderer shutdown: " + std::string(err.what()));
        }
    }
    gpu_stats_pool_.reset();
    if (pipeline_) {
        pipeline_->Shutdown();
        pipeline_.reset();
    }
    bootstrap_ = nullptr;
}

void Renderer::RenderFrame(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                   VulkanEngine::ComponentRegistry& registry,
                                   const VulkanEngine::Components::Camera& camera,
                                   VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr,
                                   VulkanEngine::BindlessManager::BindlessManager& bindless_mgr,
                                   VulkanEngine::SceneRenderer::SceneRenderer& scene_renderer,
                                   VulkanEngine::ImGui::ImGuiSystem* imgui,
                                   std::uint32_t image_index) {
    if (!pipeline_ || !pipeline_->IsCompiled()) return;

    current_registry_ = &registry;
    current_camera_ = &camera;
    current_technique_mgr_ = &technique_mgr;
    current_bindless_mgr_ = &bindless_mgr;
    current_scene_renderer_ = &scene_renderer;
    current_imgui_ = imgui;
    current_image_index_ = image_index;

    (void)bootstrap.GetBackend().GetSwapchainExtent(current_width_, current_height_);

    LOGIFACE_LOG(trace, "RenderFrame frame=" + std::to_string(frame_counter_) +
                 " img=" + std::to_string(image_index) + " w=" + std::to_string(current_width_) +
                 " h=" + std::to_string(current_height_));

    if (imgui && imgui->IsInitialized()) {
        imgui->NewFrame();
    }

    auto& backend = bootstrap.GetBackend();
    const std::uint32_t sc_count = bootstrap.GetSnapshot().swapchain_image_count;
    if (sc_count != last_swapchain_image_count_) {
        last_swapchain_image_count_ = sc_count;
        if (current_imgui_ && current_imgui_->IsInitialized()) {
            current_imgui_->OnSwapchainRecreated(sc_count,
                static_cast<vk::Format>(bootstrap.GetBackend().GetSurfaceFormat().format));
        }
    }

    // Enable GPU stats
    const std::uint32_t frame_idx = bootstrap.GetSnapshot().frame_index;
    auto& cmd = backend.GetCommandBuffer(frame_idx);
    cmd.reset({});
    cmd.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    if (gpu_stats_pool_) {
        auto& device = backend.GetDevice();
        auto* dev_dispatcher = device.getDispatcher();
        std::array<uint64_t, 8> stats{};
        const vk::Result qr = static_cast<vk::Result>(dev_dispatcher->vkGetQueryPoolResults(
            static_cast<vk::Device::CType>(*device),
            static_cast<vk::QueryPool::CType>(**gpu_stats_pool_),
            0, 1,
            sizeof(stats), stats.data(),
            sizeof(uint64_t),
            static_cast<vk::QueryResultFlags::MaskType>(vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability)));
        if (qr == vk::Result::eSuccess && stats[7] != 0) {
            LOGIFACE_LOG(trace,
                "GPU frame=" + std::to_string(frame_counter_) +
                " IA_verts=" + std::to_string(stats[0]) +
                " IA_prims=" + std::to_string(stats[1]) +
                " VS_invoc=" + std::to_string(stats[2]) +
                " clip_invoc=" + std::to_string(stats[3]) +
                " clip_prims=" + std::to_string(stats[4]) +
                " FS_invoc=" + std::to_string(stats[5]) +
                " CS_invoc=" + std::to_string(stats[6]));
        }
        cmd.resetQueryPool(**gpu_stats_pool_, 0, 1);
        cmd.beginQuery(**gpu_stats_pool_, 0, {});
    }

    // Phase 1: CPU gather + upload + descriptor writes (before render graph)
    if (current_scene_renderer_ && current_registry_) {
        const float aspect = static_cast<float>(current_width_) / static_cast<float>(current_height_);
        const glm::mat4 view = current_camera_->GetViewMatrix();
        const glm::mat4 proj = current_camera_->GetProjectionMatrix(aspect);
        current_view_proj_ = proj * view;

        // Bind actual depth to Hi-Z descriptor before hiz-gen pass executes
        const auto& depth_view = backend.GetDepthImageView(image_index);
        current_scene_renderer_->UpdateHizDepthBinding(frame_counter_, *depth_view);

        // Initialize Hi-Z on first frame
        current_scene_renderer_->InitializeHizFirstFrame(cmd);

        // CPU gather + upload + descriptor writes for all passes
        current_scene_renderer_->PrepareCompute(cmd, *current_registry_,
                                                view, proj,
                                                current_width_, current_height_,
                                                frame_counter_);
    }

    // Phase 2: Render graph executes all GPU passes in dependency order
    pipeline_->Execute(nullptr, cmd, image_index);

    if (gpu_stats_pool_) {
        cmd.endQuery(**gpu_stats_pool_, 0);
    }

    cmd.end();

    frame_counter_++;

    current_registry_ = nullptr;
    current_camera_ = nullptr;
    current_technique_mgr_ = nullptr;
    current_bindless_mgr_ = nullptr;
    current_scene_renderer_ = nullptr;
    current_imgui_ = nullptr;
}

} // namespace VulkanEngine::Renderer
