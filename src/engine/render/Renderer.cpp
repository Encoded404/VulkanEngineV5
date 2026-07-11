module;
#include <glm/glm.hpp>

#include <glm/gtc/matrix_transform.hpp> //NOLINT(misc-include-cleaner)

#include <logging/logging_macros.hpp>

module VulkanEngine.Renderer;

import std;
import std.compat;

import logiface;

import vulkan_hpp;

import VulkanBackend.Vulkan.VulkanBootstrap;
import VulkanBackend.Vulkan.VulkanDebugUtils;
import VulkanEngine.RenderGraph;
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

bool Renderer::Initialize(VulkanBackend::Vulkan::VulkanBootstrap& bootstrap,
                                  const RendererConfig& config,
                                  VulkanEngine::SceneRenderer::SceneRenderer& scene_renderer) {
    bootstrap_ = &bootstrap;
    scene_renderer_ = &scene_renderer;

    pipeline_ = std::make_unique<VulkanEngine::RenderPipeline::RenderPipeline>();
    pipeline_->Initialize(bootstrap);

    // Create pass class instances
    expand_pass_ = std::make_unique<VulkanEngine::SceneRenderer::ExpandPass>(scene_renderer);
    depth_pass_ = std::make_unique<VulkanEngine::SceneRenderer::DepthPrePass>(scene_renderer);
    hiz_pass_ = std::make_unique<VulkanEngine::SceneRenderer::HiZPass>(scene_renderer);
    occlusion_pass_ = std::make_unique<VulkanEngine::SceneRenderer::OcclusionPass>(scene_renderer);
    collect_pass_ = std::make_unique<VulkanEngine::SceneRenderer::CollectPass>(scene_renderer);
    main_pass_ = std::make_unique<VulkanEngine::SceneRenderer::MainPass>(scene_renderer);

    auto backbuffer = pipeline_->ImportBackbuffer();
    auto depth_buffer = pipeline_->ImportDepthBuffer();
    auto hiz_image = pipeline_->ImportImage("hiz-image");
    auto scene_buffers = pipeline_->ImportBuffer("scene-buffers");
    auto draw_indirect = pipeline_->ImportBuffer("draw-indirect");

    pipeline_->RegisterResourceResolver("hiz-image",
        [this](std::uint32_t) { return current_ctx_ ? current_ctx_->scene_renderer.GetHizImage(current_ctx_->frame_counter) : nullptr; },
        [this](std::uint32_t) { return current_ctx_ ? current_ctx_->scene_renderer.GetHizFullView(current_ctx_->frame_counter) : nullptr; },
        vk::Format::eR32Sfloat);

    pipeline_->SetFinalState(
        backbuffer,
        VulkanEngine::RenderGraph::ResourceState::ImageState(
            VulkanEngine::RenderGraph::PipelineStageIntent::BottomOfPipe,
            VulkanEngine::RenderGraph::AccessIntent::None,
            VulkanEngine::RenderGraph::QueueType::Graphics,
            VulkanEngine::RenderGraph::ImageLayoutIntent::Present));

    // ── Pass 1: Expand (compute) ──
    auto expand_handle = pipeline_->AddPass({
        .name = "expand",
        .queue = VulkanEngine::RenderGraph::QueueType::Graphics,
        .writes = {scene_buffers, draw_indirect},
        .execute = [this](const void* user_data, vk::CommandBuffer cmd) {
            auto& fctx = *static_cast<const FrameRenderContext*>(user_data);
            VulkanEngine::PipelinePass::FrameContext pctx{};
            pctx.render_extent = vk::Extent2D{fctx.width, fctx.height};
            pctx.frame_index = fctx.frame_counter;
            pctx.view_proj = fctx.view_proj;
            pctx.entity_count = scene_renderer_->GetCurrentEntityCount();
            pctx.render_width = fctx.width;
            pctx.render_height = fctx.height;
            expand_pass_->Execute(pctx, cmd);
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

    auto depth_handle = pipeline_->AddPass({
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
        .execute = [this](const void* user_data, vk::CommandBuffer cmd) {
            auto& fctx = *static_cast<const FrameRenderContext*>(user_data);
            VulkanEngine::PipelinePass::FrameContext pctx{};
            pctx.render_extent = vk::Extent2D{fctx.width, fctx.height};
            pctx.frame_index = fctx.frame_counter;
            pctx.render_width = fctx.width;
            pctx.render_height = fctx.height;
            depth_pass_->Execute(pctx, cmd);
        }
    });

    // ── Pass 3: Hi-Z generation compute ──
    auto hiz_handle = pipeline_->AddPass({
        .name = "hiz-gen",
        .queue = VulkanEngine::RenderGraph::QueueType::Graphics,
        .reads = {{depth_buffer,
            VulkanEngine::RenderGraph::PipelineStageIntent::ComputeShader,
            VulkanEngine::RenderGraph::AccessIntent::Read}},
        .writes = {hiz_image},
        .execute = [this](const void* user_data, vk::CommandBuffer cmd) {
            auto& fctx = *static_cast<const FrameRenderContext*>(user_data);
            VulkanEngine::PipelinePass::FrameContext pctx{};
            pctx.render_extent = vk::Extent2D{fctx.width, fctx.height};
            pctx.frame_index = fctx.frame_counter;
            pctx.swapchain_image_index = fctx.image_index;
            pctx.render_width = fctx.width;
            pctx.render_height = fctx.height;
            hiz_pass_->Execute(pctx, cmd);
        }
    });

    // ── Pass 4: Occlusion cull compute ──
    auto occlusion_handle = pipeline_->AddPass({
        .name = "occlusion",
        .queue = VulkanEngine::RenderGraph::QueueType::Graphics,
        .reads = {{hiz_image,
            VulkanEngine::RenderGraph::PipelineStageIntent::ComputeShader,
            VulkanEngine::RenderGraph::AccessIntent::Read},
            {scene_buffers,
            VulkanEngine::RenderGraph::PipelineStageIntent::ComputeShader,
            VulkanEngine::RenderGraph::AccessIntent::Read}},
        .writes = {scene_buffers},
        .execute = [this](const void* user_data, vk::CommandBuffer cmd) {
            auto& fctx = *static_cast<const FrameRenderContext*>(user_data);
            VulkanEngine::PipelinePass::FrameContext pctx{};
            pctx.frame_index = fctx.frame_counter;
            occlusion_pass_->Execute(pctx, cmd);
        }
    });

    // ── Pass 5: Collect compute (count + compact + draw commands) ──
    auto collect_handle = pipeline_->AddPass({
        .name = "collect",
        .queue = VulkanEngine::RenderGraph::QueueType::Graphics,
        .reads = {{scene_buffers,
            VulkanEngine::RenderGraph::PipelineStageIntent::ComputeShader,
            VulkanEngine::RenderGraph::AccessIntent::Read}},
        .writes = {scene_buffers, draw_indirect},
        .execute = [this](const void* user_data, vk::CommandBuffer cmd) {
            auto& fctx = *static_cast<const FrameRenderContext*>(user_data);
            VulkanEngine::PipelinePass::FrameContext pctx{};
            pctx.frame_index = fctx.frame_counter;
            collect_pass_->Execute(pctx, cmd);
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

    auto main_handle = pipeline_->AddPass({
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
        .execute = [](const void* user_data, vk::CommandBuffer cmd) {
            auto& fctx = *static_cast<const FrameRenderContext*>(user_data);
            const float aspect = static_cast<float>(fctx.width) / static_cast<float>(fctx.height);
            const glm::mat4 view = fctx.camera.GetViewMatrix();
            const glm::mat4 proj = fctx.camera.GetProjectionMatrix(aspect);

            fctx.scene_renderer.Render(cmd, fctx.registry,
                                       fctx.technique_mgr, fctx.bindless_mgr,
                                       proj, view,
                                       fctx.width, fctx.height,
                                       fctx.frame_counter);
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
            .execute = [this](const void* user_data, vk::CommandBuffer cmd) {
                auto& ctx = *static_cast<const FrameRenderContext*>(user_data);
                if (ctx.imgui && ctx.imgui->IsInitialized()) {
                    auto& backend = bootstrap_->GetBackend();
                    ctx.imgui->RenderDrawData(cmd,
                        *backend.GetSwapchainImageViews()[ctx.image_index],
                        ctx.width, ctx.height);
                }
            }
        });
    }

    // Explicit ordering ensures correct pipeline
    pipeline_->AddDependency(expand_handle, depth_handle);
    pipeline_->AddDependency(depth_handle, hiz_handle);
    pipeline_->AddDependency(hiz_handle, occlusion_handle);
    pipeline_->AddDependency(occlusion_handle, collect_handle);
    pipeline_->AddDependency(collect_handle, main_handle);

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
        VulkanBackend::Vulkan::SetVulkanObjectName(device, *gpu_stats_pool_, "gpu-stats-pool");
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

void Renderer::RenderFrame(VulkanBackend::Vulkan::VulkanBootstrap& bootstrap,
                                   VulkanEngine::ComponentRegistry& registry,
                                   const VulkanEngine::Components::Camera& camera,
                                   VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr,
                                   VulkanEngine::BindlessManager::BindlessManager& bindless_mgr,
                                   VulkanEngine::SceneRenderer::SceneRenderer& scene_renderer,
                                   VulkanEngine::ImGui::ImGuiSystem* imgui,
                                   std::uint32_t image_index) {
    if (!pipeline_ || !pipeline_->IsCompiled()) return;

    std::uint32_t width = 0, height = 0;
    (void)bootstrap.GetBackend().GetSwapchainExtent(width, height);

    LOGIFACE_LOG(trace, "RenderFrame frame=" + std::to_string(frame_counter_) +
                 " img=" + std::to_string(image_index) + " w=" + std::to_string(width) +
                 " h=" + std::to_string(height));

    if (imgui && imgui->IsInitialized()) {
        imgui->NewFrame();
    }

    auto& backend = bootstrap.GetBackend();
    const std::uint32_t sc_count = bootstrap.GetSnapshot().swapchain_image_count;
    if (sc_count != last_swapchain_image_count_) {
        last_swapchain_image_count_ = sc_count;
        if (imgui && imgui->IsInitialized()) {
            imgui->OnSwapchainRecreated(sc_count,
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
    {
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        const glm::mat4 view = camera.GetViewMatrix();
        const glm::mat4 proj = camera.GetProjectionMatrix(aspect);
        const glm::mat4 view_proj = proj * view;

        // Bind actual depth to Hi-Z descriptor before hiz-gen pass executes
        const auto& depth_view = backend.GetDepthImageView(image_index);
        scene_renderer.UpdateHizDepthBinding(frame_counter_, *depth_view);

        // Initialize Hi-Z on first frame
        scene_renderer.InitializeHizFirstFrame(cmd);

        // CPU gather + upload + descriptor writes for all passes
        scene_renderer.PrepareCompute(cmd, registry, view, proj, width, height, frame_counter_);

        // Create per-frame context and set it for the resource resolver
        FrameRenderContext ctx{
            .registry = registry,
            .camera = camera,
            .technique_mgr = technique_mgr,
            .bindless_mgr = bindless_mgr,
            .scene_renderer = scene_renderer,
            .imgui = imgui,
            .width = width,
            .height = height,
            .image_index = image_index,
            .frame_counter = frame_counter_,
            .view_proj = view_proj,
        };
        current_ctx_ = &ctx;

        // Phase 2: Render graph executes all GPU passes in dependency order
        pipeline_->Execute(&ctx, cmd, image_index);
    }

    if (gpu_stats_pool_) {
        cmd.endQuery(**gpu_stats_pool_, 0);
    }

    cmd.end();

    frame_counter_++;

    current_ctx_ = nullptr;
}

} // namespace VulkanEngine::Renderer
