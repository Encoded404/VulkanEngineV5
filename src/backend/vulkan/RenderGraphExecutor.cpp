module;

module VulkanBackend.Vulkan.RenderGraphExecutor;

import std;
import std.compat;

import vulkan_hpp;

import VulkanShared.RenderGraphTypes;

using VulkanEngine::RenderGraph::ResourceState;
using VulkanEngine::RenderGraph::PipelineStageIntent;
using VulkanEngine::RenderGraph::AccessIntent;
using VulkanEngine::RenderGraph::ImageLayoutIntent;

namespace VulkanBackend::Vulkan {

namespace {

static void BeginPassLabel(vk::CommandBuffer cmd, const std::string& name) {
    vk::DebugUtilsLabelEXT label{};
    label.pLabelName = name.c_str();
    cmd.beginDebugUtilsLabelEXT(label);
}

static void EndPassLabel(vk::CommandBuffer cmd) {
    cmd.endDebugUtilsLabelEXT();
}

} // anonymous namespace

void ExecuteRenderGraph(const VulkanEngine::RenderGraph::CompiledRenderGraph& graph,
                         const void* user_data,
                         vk::CommandBuffer command_buffer) {
    if (!graph.success) {
        return;
    }

    std::vector<ResourceState> current_states = graph.initial_states;
    std::vector<bool> has_state = graph.has_initial_state;

    for (const auto& pass : graph.passes) {
        BeginPassLabel(command_buffer, pass.name);

        if (!pass.pre_pass_transitions.empty()) {
            std::vector<vk::ImageMemoryBarrier> image_barriers;
            std::vector<vk::MemoryBarrier> memory_barriers;
            vk::PipelineStageFlags src_stage = vk::PipelineStageFlagBits::eTopOfPipe;
            vk::PipelineStageFlags dst_stage = vk::PipelineStageFlagBits::eTopOfPipe;

            for (const auto& transition : pass.pre_pass_transitions) {
                const auto& res_info = graph.resource_info[transition.resource_index];

                if (res_info.kind == VulkanEngine::RenderGraph::ResourceKind::Image) {
                    if (!has_state[transition.resource_index]) {
                        const ResourceState undefined_state = ResourceState::ImageState(
                            PipelineStageIntent::TopOfPipe, AccessIntent::None,
                            VulkanEngine::RenderGraph::QueueType::Graphics, ImageLayoutIntent::Undefined);
                        current_states[transition.resource_index] = undefined_state;
                        has_state[transition.resource_index] = true;
                    }

                    const auto& from = current_states[transition.resource_index];
                    const auto& to = transition.target_state;

                    if (!VulkanEngine::RenderGraph::StatesEqual(from, to)) {
                        vk::ImageMemoryBarrier barrier{};
                        barrier.image = graph.resource_images[transition.resource_index];
                        barrier.srcAccessMask = VulkanEngine::RenderGraph::IntentToAccessFlags(from.stage, from.access);
                        barrier.dstAccessMask = VulkanEngine::RenderGraph::IntentToAccessFlags(to.stage, to.access);
                        barrier.oldLayout = VulkanEngine::RenderGraph::IntentToImageLayout(from.layout);
                        barrier.newLayout = VulkanEngine::RenderGraph::IntentToImageLayout(to.layout);
                        barrier.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
                        barrier.dstQueueFamilyIndex = vk::QueueFamilyIgnored;

                        const auto format = graph.resource_formats[transition.resource_index];
                        barrier.subresourceRange = {
                            VulkanEngine::RenderGraph::FormatToAspectFlags(format),
                            0, vk::RemainingMipLevels,
                            0, vk::RemainingArrayLayers
                        };

                        src_stage |= VulkanEngine::RenderGraph::IntentToPipelineStage(from.stage, from.access);
                        dst_stage |= VulkanEngine::RenderGraph::IntentToPipelineStage(to.stage, to.access);

                        image_barriers.push_back(barrier);
                        current_states[transition.resource_index] = to;
                    }
                } else {
                    if (!has_state[transition.resource_index]) {
                        const ResourceState undefined_state = ResourceState::BufferState(
                            PipelineStageIntent::TopOfPipe, AccessIntent::None, VulkanEngine::RenderGraph::QueueType::Graphics);
                        current_states[transition.resource_index] = undefined_state;
                        has_state[transition.resource_index] = true;
                    }

                    const auto& from = current_states[transition.resource_index];
                    const auto& to = transition.target_state;

                    if (!VulkanEngine::RenderGraph::StatesEqual(from, to)) {
                        vk::MemoryBarrier barrier{};
                        barrier.srcAccessMask = VulkanEngine::RenderGraph::IntentToAccessFlags(from.stage, from.access);
                        barrier.dstAccessMask = VulkanEngine::RenderGraph::IntentToAccessFlags(to.stage, to.access);

                        src_stage |= VulkanEngine::RenderGraph::IntentToPipelineStage(from.stage, from.access);
                        dst_stage |= VulkanEngine::RenderGraph::IntentToPipelineStage(to.stage, to.access);

                        memory_barriers.push_back(barrier);
                        current_states[transition.resource_index] = to;
                    }
                }
            }

            if (dst_stage & vk::PipelineStageFlagBits::eBottomOfPipe) {
                dst_stage &= ~vk::PipelineStageFlagBits::eBottomOfPipe;
                if (dst_stage == vk::PipelineStageFlags{}) {
                    dst_stage = vk::PipelineStageFlagBits::eTopOfPipe;
                }
                for (auto& b : image_barriers) b.dstAccessMask = {};
                for (auto& b : memory_barriers) b.dstAccessMask = {};
            }
            if (src_stage & vk::PipelineStageFlagBits::eBottomOfPipe) {
                src_stage &= ~vk::PipelineStageFlagBits::eBottomOfPipe;
                if (src_stage == vk::PipelineStageFlags{}) {
                    src_stage = vk::PipelineStageFlagBits::eTopOfPipe;
                }
                for (auto& b : image_barriers) b.srcAccessMask = {};
                for (auto& b : memory_barriers) b.srcAccessMask = {};
            }
            if (dst_stage == vk::PipelineStageFlagBits::eTopOfPipe) {
                for (auto& b : image_barriers) b.dstAccessMask = {};
                for (auto& b : memory_barriers) b.dstAccessMask = {};
            }
            if (src_stage == vk::PipelineStageFlagBits::eTopOfPipe) {
                for (auto& b : image_barriers) b.srcAccessMask = {};
                for (auto& b : memory_barriers) b.srcAccessMask = {};
            }

            if (!image_barriers.empty() || !memory_barriers.empty()) {
                command_buffer.pipelineBarrier(src_stage, dst_stage, {}, memory_barriers, {}, image_barriers);
            }
        }

        if (pass.attachment_setup.has_value()) {
            if (pass.attachment_setup->auto_begin_rendering) {
                const auto& setup = *pass.attachment_setup;

                std::vector<vk::RenderingAttachmentInfo> color_attachments;
                color_attachments.reserve(setup.color_attachments.size());
                for (const auto& attach : setup.color_attachments) {
                    vk::RenderingAttachmentInfo info{};
                    info.imageView = attach.image_view;
                    info.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
                    info.loadOp = attach.load_op;
                    info.storeOp = attach.store_op;
                    if (attach.load_op == vk::AttachmentLoadOp::eClear) {
                        info.clearValue = vk::ClearValue(attach.clear_color);
                    }
                    color_attachments.push_back(info);
                }

                std::optional<vk::RenderingAttachmentInfo> depth_attachment;
                if (setup.depth_attachment.has_value()) {
                    depth_attachment = vk::RenderingAttachmentInfo{};
                    depth_attachment->imageView = setup.depth_attachment->image_view;
                    depth_attachment->imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
                    depth_attachment->loadOp = setup.depth_attachment->load_op;
                    depth_attachment->storeOp = setup.depth_attachment->store_op;
                    if (setup.depth_attachment->load_op == vk::AttachmentLoadOp::eClear) {
                        depth_attachment->clearValue = vk::ClearValue(setup.depth_attachment->clear_depth);
                    }
                }

                vk::RenderingInfo render_info{};
                render_info.renderArea = setup.render_area;
                render_info.layerCount = 1;
                render_info.colorAttachmentCount = static_cast<std::uint32_t>(color_attachments.size());
                render_info.pColorAttachments = color_attachments.data();
                if (depth_attachment) {
                    render_info.pDepthAttachment = &*depth_attachment;
                }

                command_buffer.beginRendering(render_info);
            }
        }

        if (pass.execute.callback) {
            pass.execute.callback(user_data, command_buffer);
        }

        if (pass.attachment_setup.has_value() && pass.attachment_setup->auto_begin_rendering) {
            command_buffer.endRendering();
        }

        if (!pass.post_pass_transitions.empty()) {
            std::vector<vk::ImageMemoryBarrier> image_barriers;
            std::vector<vk::MemoryBarrier> memory_barriers;
            vk::PipelineStageFlags src_stage = vk::PipelineStageFlagBits::eTopOfPipe;
            vk::PipelineStageFlags dst_stage = vk::PipelineStageFlagBits::eTopOfPipe;

            for (const auto& transition : pass.post_pass_transitions) {
                const auto& res_info = graph.resource_info[transition.resource_index];

                if (res_info.kind == VulkanEngine::RenderGraph::ResourceKind::Image) {
                    if (!has_state[transition.resource_index]) {
                        const ResourceState undefined_state = ResourceState::ImageState(
                            PipelineStageIntent::TopOfPipe, AccessIntent::None,
                            VulkanEngine::RenderGraph::QueueType::Graphics, ImageLayoutIntent::Undefined);
                        current_states[transition.resource_index] = undefined_state;
                        has_state[transition.resource_index] = true;
                    }

                    const auto& from = current_states[transition.resource_index];
                    const auto& to = transition.target_state;

                    if (!VulkanEngine::RenderGraph::StatesEqual(from, to)) {
                        vk::ImageMemoryBarrier barrier{};
                        barrier.image = graph.resource_images[transition.resource_index];
                        barrier.srcAccessMask = VulkanEngine::RenderGraph::IntentToAccessFlags(from.stage, from.access);
                        barrier.dstAccessMask = VulkanEngine::RenderGraph::IntentToAccessFlags(to.stage, to.access);
                        barrier.oldLayout = VulkanEngine::RenderGraph::IntentToImageLayout(from.layout);
                        barrier.newLayout = VulkanEngine::RenderGraph::IntentToImageLayout(to.layout);
                        barrier.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
                        barrier.dstQueueFamilyIndex = vk::QueueFamilyIgnored;

                        const auto format = graph.resource_formats[transition.resource_index];
                        barrier.subresourceRange = {
                            VulkanEngine::RenderGraph::FormatToAspectFlags(format),
                            0, vk::RemainingMipLevels,
                            0, vk::RemainingArrayLayers
                        };

                        src_stage |= VulkanEngine::RenderGraph::IntentToPipelineStage(from.stage, from.access);
                        dst_stage |= VulkanEngine::RenderGraph::IntentToPipelineStage(to.stage, to.access);

                        image_barriers.push_back(barrier);
                        current_states[transition.resource_index] = to;
                    }
                } else {
                    if (!has_state[transition.resource_index]) {
                        const ResourceState undefined_state = ResourceState::BufferState(
                            PipelineStageIntent::TopOfPipe, AccessIntent::None, VulkanEngine::RenderGraph::QueueType::Graphics);
                        current_states[transition.resource_index] = undefined_state;
                        has_state[transition.resource_index] = true;
                    }

                    const auto& from = current_states[transition.resource_index];
                    const auto& to = transition.target_state;

                    if (!VulkanEngine::RenderGraph::StatesEqual(from, to)) {
                        vk::MemoryBarrier barrier{};
                        barrier.srcAccessMask = VulkanEngine::RenderGraph::IntentToAccessFlags(from.stage, from.access);
                        barrier.dstAccessMask = VulkanEngine::RenderGraph::IntentToAccessFlags(to.stage, to.access);

                        src_stage |= VulkanEngine::RenderGraph::IntentToPipelineStage(from.stage, from.access);
                        dst_stage |= VulkanEngine::RenderGraph::IntentToPipelineStage(to.stage, to.access);

                        memory_barriers.push_back(barrier);
                        current_states[transition.resource_index] = to;
                    }
                }
            }

            if (dst_stage & vk::PipelineStageFlagBits::eBottomOfPipe) {
                dst_stage &= ~vk::PipelineStageFlagBits::eBottomOfPipe;
                if (dst_stage == vk::PipelineStageFlags{}) {
                    dst_stage = vk::PipelineStageFlagBits::eTopOfPipe;
                }
                for (auto& b : image_barriers) b.dstAccessMask = {};
                for (auto& b : memory_barriers) b.dstAccessMask = {};
            }
            if (src_stage & vk::PipelineStageFlagBits::eBottomOfPipe) {
                src_stage &= ~vk::PipelineStageFlagBits::eBottomOfPipe;
                if (src_stage == vk::PipelineStageFlags{}) {
                    src_stage = vk::PipelineStageFlagBits::eTopOfPipe;
                }
                for (auto& b : image_barriers) b.srcAccessMask = {};
                for (auto& b : memory_barriers) b.srcAccessMask = {};
            }
            if (dst_stage == vk::PipelineStageFlagBits::eTopOfPipe) {
                for (auto& b : image_barriers) b.dstAccessMask = {};
                for (auto& b : memory_barriers) b.dstAccessMask = {};
            }
            if (src_stage == vk::PipelineStageFlagBits::eTopOfPipe) {
                for (auto& b : image_barriers) b.srcAccessMask = {};
                for (auto& b : memory_barriers) b.srcAccessMask = {};
            }

            if (!image_barriers.empty() || !memory_barriers.empty()) {
                command_buffer.pipelineBarrier(src_stage, dst_stage, {}, memory_barriers, {}, image_barriers);
            }
        }

        EndPassLabel(command_buffer);
    }
}

} // namespace VulkanBackend::Vulkan
