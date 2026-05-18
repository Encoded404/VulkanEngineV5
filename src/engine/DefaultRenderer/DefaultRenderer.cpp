module;

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/matrix_transform.hpp> //NOLINT(misc-include-cleaner)
#include <cstdint>
#include <array>
#include <memory>

#include <vulkan/vulkan.hpp>
#include <logging/logging.hpp>

module VulkanEngine.DefaultRenderer;

import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanBackend.RenderGraph;
import VulkanEngine.RenderPipeline;
import VulkanEngine.SceneRenderer;
import VulkanEngine.TechniqueManager;
import VulkanEngine.BindlessManager;
import VulkanEngine.Components.Camera;
import VulkanEngine.GpuResources;
import VulkanEngine.ImGuiSystem;

namespace VulkanEngine::DefaultRenderer {

DefaultRenderer::~DefaultRenderer() { // NOLINT(bugprone-exception-escape)
    Shutdown();
}

bool DefaultRenderer::Initialize(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                  const DefaultRendererConfig& config) {
    bootstrap_ = &bootstrap;

    pipeline_ = std::make_unique<VulkanEngine::RenderPipeline::RenderPipeline>();
    pipeline_->Initialize(bootstrap);

    auto backbuffer = pipeline_->ImportBackbuffer();
    auto depth_buffer = pipeline_->ImportDepthBuffer();

    pipeline_->SetFinalState(
        backbuffer,
        VulkanEngine::RenderGraph::ResourceState::ImageState(
            VulkanEngine::RenderGraph::PipelineStageIntent::BottomOfPipe,
            VulkanEngine::RenderGraph::AccessIntent::None,
            VulkanEngine::RenderGraph::QueueType::Graphics,
            VulkanEngine::RenderGraph::ImageLayoutIntent::Present));

    VulkanEngine::RenderGraph::PassAttachmentSetup attachment_setup{};
    attachment_setup.auto_begin_rendering = true;

    VulkanEngine::RenderGraph::AttachmentInfo color_attach{};
    color_attach.resource = backbuffer;
    color_attach.load_op = vk::AttachmentLoadOp::eClear;
    color_attach.store_op = vk::AttachmentStoreOp::eStore;
    color_attach.clear_color = vk::ClearColorValue(std::array<float, 4>{
        config.clear_color.r, config.clear_color.g, config.clear_color.b, config.clear_color.a});
    attachment_setup.color_attachments.push_back(color_attach);

    VulkanEngine::RenderGraph::AttachmentInfo depth_attach{};
    depth_attach.resource = depth_buffer;
    depth_attach.load_op = vk::AttachmentLoadOp::eClear;
    depth_attach.store_op = vk::AttachmentStoreOp::eDontCare;
    depth_attach.clear_depth = config.clear_depth_stencil;
    attachment_setup.depth_attachment = depth_attach;

    VulkanEngine::RenderPipeline::RenderPipelinePassDesc render_pass_desc{};
    render_pass_desc.name = "default-render";
    render_pass_desc.queue = VulkanEngine::RenderGraph::QueueType::Graphics;
    render_pass_desc.writes = {backbuffer, depth_buffer};
    render_pass_desc.attachments = attachment_setup;
    render_pass_desc.execute = [this](const void*, vk::CommandBuffer cmd) {
        if (current_scene_renderer_) {
            const float aspect = static_cast<float>(current_width_) / static_cast<float>(current_height_);
            const glm::mat4 view = current_camera_->GetViewMatrix();
            const glm::mat4 proj = current_camera_->GetProjectionMatrix(aspect);

            current_scene_renderer_->Render(cmd, *current_registry_,
                                            *current_vertex_buffer_, *current_index_buffer_,
                                            *current_technique_mgr_, *current_bindless_mgr_,
                                            proj, view,
                                            current_width_, current_height_,
                                            frame_counter_);
        }
    };

    pipeline_->AddPass(render_pass_desc);

    if (config.enable_imgui) {
        VulkanEngine::RenderPipeline::RenderPipelinePassDesc imgui_pass_desc{};
        imgui_pass_desc.name = "imgui-overlay";
        imgui_pass_desc.queue = VulkanEngine::RenderGraph::QueueType::Graphics;
        imgui_pass_desc.writes = {backbuffer};
        imgui_pass_desc.execute = [this](const void*, vk::CommandBuffer cmd) {
            if (current_imgui_ && current_imgui_->IsInitialized()) {
                auto& backend = bootstrap_->GetBackend();
                current_imgui_->RenderDrawData(cmd,
                    *backend.GetSwapchainImageViews()[current_image_index_],
                    current_width_, current_height_);
            }
        };
        pipeline_->AddPass(imgui_pass_desc);
    }

    pipeline_->Compile();
    if (!pipeline_->IsCompiled()) return false;

    {
        auto& device = bootstrap.GetBackend().GetDevice();
        vk::QueryPoolCreateInfo qp_info{};
        qp_info.queryType = vk::QueryType::ePipelineStatistics;
        qp_info.pipelineStatistics = GPU_STATS_FLAGS;
        qp_info.queryCount = 1;
        gpu_stats_pool_ = std::make_unique<vk::raii::QueryPool>(device, qp_info);
        vkResetQueryPool(*device, **gpu_stats_pool_, 0, 1);
    }

    LOGIFACE_LOG(info, "DefaultRenderer initialized");
    return true;
}

void DefaultRenderer::Shutdown() {
    if (bootstrap_) {
        bootstrap_->GetBackend().GetDevice().waitIdle();
    }
    gpu_stats_pool_.reset();
    if (pipeline_) {
        pipeline_->Shutdown();
        pipeline_.reset();
    }
    bootstrap_ = nullptr;
}

void DefaultRenderer::RenderFrame(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                   VulkanEngine::ComponentRegistry& registry,
                                   const VulkanEngine::Components::Camera& camera,
                                   const VulkanEngine::GpuResources::GpuBuffer& vertex_buffer,
                                   const VulkanEngine::GpuResources::GpuBuffer& index_buffer,
                                   VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr,
                                   VulkanEngine::BindlessManager::BindlessManager& bindless_mgr,
                                   VulkanEngine::SceneRenderer::SceneRenderer& scene_renderer,
                                   VulkanEngine::ImGuiSystem::ImGuiSystem* imgui,
                                   uint32_t image_index) {
    if (!pipeline_ || !pipeline_->IsCompiled()) return;

    current_registry_ = &registry;
    current_camera_ = &camera;
    current_vertex_buffer_ = &vertex_buffer;
    current_index_buffer_ = &index_buffer;
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
    const uint32_t frame_idx = bootstrap.GetSnapshot().frame_index;
    auto& cmd = backend.GetCommandBuffer(frame_idx);
    cmd.reset({});
    cmd.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    if (gpu_stats_pool_) {
        // Read back previous frame's GPU pipeline statistics
        std::array<uint64_t, 7> gpu_stats{};
        VkQueryPool pool = **gpu_stats_pool_;
        VkDevice dev = *backend.GetDevice();
        auto* fn = reinterpret_cast<PFN_vkGetQueryPoolResults>(
            vkGetDeviceProcAddr(dev, "vkGetQueryPoolResults"));
        if (fn) {
            VkResult result = fn(dev, pool, 0, 1,
                sizeof(gpu_stats), gpu_stats.data(), sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT);
            if (result == VK_SUCCESS) {
                LOGIFACE_LOG(trace, "GPU STATS: IA_vertices=" + std::to_string(gpu_stats[0]) +
                             " IA_primitives=" + std::to_string(gpu_stats[1]) +
                             " VS_invocations=" + std::to_string(gpu_stats[2]) +
                             " clipping_invocations=" + std::to_string(gpu_stats[3]) +
                             " clipping_primitives=" + std::to_string(gpu_stats[4]) +
                             " FS_invocations=" + std::to_string(gpu_stats[5]) +
                             " compute_invocations=" + std::to_string(gpu_stats[6]));
            }
        }

        cmd.resetQueryPool(**gpu_stats_pool_, 0, 1);
        cmd.beginQuery(**gpu_stats_pool_, 0, {});
    }

    LOGIFACE_LOG(trace, "Pre-compute: frustum culling");
    if (current_scene_renderer_ && current_registry_) {
        current_scene_renderer_->PrepareCompute(cmd, *current_registry_, frame_counter_);
    }

    pipeline_->Execute(nullptr, cmd, image_index);

    if (gpu_stats_pool_) {
        cmd.endQuery(**gpu_stats_pool_, 0);
    }

    cmd.end();

    frame_counter_++;

    current_registry_ = nullptr;
    current_camera_ = nullptr;
    current_vertex_buffer_ = nullptr;
    current_index_buffer_ = nullptr;
    current_technique_mgr_ = nullptr;
    current_bindless_mgr_ = nullptr;
    current_scene_renderer_ = nullptr;
    current_imgui_ = nullptr;
}

}
