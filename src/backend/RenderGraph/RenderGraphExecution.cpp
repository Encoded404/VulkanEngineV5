module;

#include <cstdint>
#include <optional>
#include <vector>

#include <vulkan/vulkan.hpp>

module VulkanBackend.RenderGraph;

namespace VulkanEngine::RenderGraph {

void CompiledRenderGraph::SetImportedResourceState(uint32_t resource_index, ResourceState state) const {
    if (resource_index < initial_states.size()) {
        initial_states[resource_index] = state;
        has_initial_state[resource_index] = true;
    }
}

void CompiledRenderGraph::SetResourceImage(uint32_t resource_index, vk::Image image) {
    if (resource_index < resource_images.size()) {
        resource_images[resource_index] = image;
    }
}

void CompiledRenderGraph::SetResourceFormat(uint32_t resource_index, vk::Format format) {
    if (resource_index < resource_formats.size()) {
        resource_formats[resource_index] = format;
    }
}

void CompiledRenderGraph::Execute(const void* user_data, vk::CommandBuffer command_buffer) const {
    if (!success) {
        return;
    }

    std::vector<ResourceState> current_states = initial_states;
    std::vector<bool> has_state = has_initial_state;

    for (const auto& pass : passes) {
        if (!pass.pre_pass_transitions.empty()) {
            std::vector<vk::ImageMemoryBarrier> image_barriers;
            std::vector<vk::BufferMemoryBarrier> buffer_barriers;
            vk::PipelineStageFlags src_stage = vk::PipelineStageFlagBits::eTopOfPipe;
            vk::PipelineStageFlags dst_stage = vk::PipelineStageFlagBits::eTopOfPipe;

            for (const auto& transition : pass.pre_pass_transitions) {
                const auto& res_info = resource_info[transition.resource_index];

                if (res_info.kind == ResourceKind::Image) {
                    if (!has_state[transition.resource_index]) {
                        const ResourceState undefined_state = ResourceState::ImageState(
                            PipelineStageIntent::TopOfPipe, AccessIntent::None,
                            QueueType::Graphics, ImageLayoutIntent::Undefined);
                        current_states[transition.resource_index] = undefined_state;
                        has_state[transition.resource_index] = true;
                    }

                    const auto& from = current_states[transition.resource_index];
                    const auto& to = transition.target_state;

                    if (!StatesEqual(from, to)) {
                        vk::ImageMemoryBarrier barrier{};
                        barrier.image = resource_images[transition.resource_index];
                        barrier.srcAccessMask = IntentToAccessFlags(from.stage, from.access);
                        barrier.dstAccessMask = IntentToAccessFlags(to.stage, to.access);
                        barrier.oldLayout = IntentToImageLayout(from.layout);
                        barrier.newLayout = IntentToImageLayout(to.layout);
                        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

                        const auto format = resource_formats[transition.resource_index];
                        barrier.subresourceRange = {
                            FormatToAspectFlags(format),
                            0, 1u,
                            0, 1u
                        };

                        src_stage |= IntentToPipelineStage(from.stage, from.access);
                        dst_stage |= IntentToPipelineStage(to.stage, to.access);

                        image_barriers.push_back(barrier);
                        current_states[transition.resource_index] = to;
                    }
                } else {
                    if (!has_state[transition.resource_index]) {
                        const ResourceState undefined_state = ResourceState::BufferState(
                            PipelineStageIntent::TopOfPipe, AccessIntent::None, QueueType::Graphics);
                        current_states[transition.resource_index] = undefined_state;
                        has_state[transition.resource_index] = true;
                    }

                    const auto& from = current_states[transition.resource_index];
                    const auto& to = transition.target_state;

                    if (!StatesEqual(from, to)) {
                        vk::BufferMemoryBarrier barrier{};
                        barrier.srcAccessMask = IntentToAccessFlags(from.stage, from.access);
                        barrier.dstAccessMask = IntentToAccessFlags(to.stage, to.access);
                        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.size = VK_WHOLE_SIZE;

                        src_stage |= IntentToPipelineStage(from.stage, from.access);
                        dst_stage |= IntentToPipelineStage(to.stage, to.access);

                        buffer_barriers.push_back(barrier);
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
                for (auto& b : buffer_barriers) b.dstAccessMask = {};
            }
            if (src_stage & vk::PipelineStageFlagBits::eBottomOfPipe) {
                src_stage &= ~vk::PipelineStageFlagBits::eBottomOfPipe;
                if (src_stage == vk::PipelineStageFlags{}) {
                    src_stage = vk::PipelineStageFlagBits::eTopOfPipe;
                }
                for (auto& b : image_barriers) b.srcAccessMask = {};
                for (auto& b : buffer_barriers) b.srcAccessMask = {};
            }
            if (dst_stage == vk::PipelineStageFlagBits::eTopOfPipe) {
                for (auto& b : image_barriers) b.dstAccessMask = {};
                for (auto& b : buffer_barriers) b.dstAccessMask = {};
            }
            if (src_stage == vk::PipelineStageFlagBits::eTopOfPipe) {
                for (auto& b : image_barriers) b.srcAccessMask = {};
                for (auto& b : buffer_barriers) b.srcAccessMask = {};
            }

            if (!image_barriers.empty() || !buffer_barriers.empty()) {
                command_buffer.pipelineBarrier(src_stage, dst_stage, {}, {}, buffer_barriers, image_barriers);
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
                render_info.colorAttachmentCount = static_cast<uint32_t>(color_attachments.size());
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
            std::vector<vk::BufferMemoryBarrier> buffer_barriers;
            vk::PipelineStageFlags src_stage = vk::PipelineStageFlagBits::eTopOfPipe;
            vk::PipelineStageFlags dst_stage = vk::PipelineStageFlagBits::eTopOfPipe;

            for (const auto& transition : pass.post_pass_transitions) {
                const auto& res_info = resource_info[transition.resource_index];

                if (res_info.kind == ResourceKind::Image) {
                    if (!has_state[transition.resource_index]) {
                        const ResourceState undefined_state = ResourceState::ImageState(
                            PipelineStageIntent::TopOfPipe, AccessIntent::None,
                            QueueType::Graphics, ImageLayoutIntent::Undefined);
                        current_states[transition.resource_index] = undefined_state;
                        has_state[transition.resource_index] = true;
                    }

                    const auto& from = current_states[transition.resource_index];
                    const auto& to = transition.target_state;

                    if (!StatesEqual(from, to)) {
                        vk::ImageMemoryBarrier barrier{};
                        barrier.image = resource_images[transition.resource_index];
                        barrier.srcAccessMask = IntentToAccessFlags(from.stage, from.access);
                        barrier.dstAccessMask = IntentToAccessFlags(to.stage, to.access);
                        barrier.oldLayout = IntentToImageLayout(from.layout);
                        barrier.newLayout = IntentToImageLayout(to.layout);
                        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

                        const auto format = resource_formats[transition.resource_index];
                        barrier.subresourceRange = {
                            FormatToAspectFlags(format),
                            0, 1u,
                            0, 1u
                        };

                        src_stage |= IntentToPipelineStage(from.stage, from.access);
                        dst_stage |= IntentToPipelineStage(to.stage, to.access);

                        image_barriers.push_back(barrier);
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
                for (auto& b : buffer_barriers) b.dstAccessMask = {};
            }
            if (src_stage & vk::PipelineStageFlagBits::eBottomOfPipe) {
                src_stage &= ~vk::PipelineStageFlagBits::eBottomOfPipe;
                if (src_stage == vk::PipelineStageFlags{}) {
                    src_stage = vk::PipelineStageFlagBits::eTopOfPipe;
                }
                for (auto& b : image_barriers) b.srcAccessMask = {};
                for (auto& b : buffer_barriers) b.srcAccessMask = {};
            }
            if (dst_stage == vk::PipelineStageFlagBits::eTopOfPipe) {
                for (auto& b : image_barriers) b.dstAccessMask = {};
                for (auto& b : buffer_barriers) b.dstAccessMask = {};
            }
            if (src_stage == vk::PipelineStageFlagBits::eTopOfPipe) {
                for (auto& b : image_barriers) b.srcAccessMask = {};
                for (auto& b : buffer_barriers) b.srcAccessMask = {};
            }

            if (!image_barriers.empty() || !buffer_barriers.empty()) {
                command_buffer.pipelineBarrier(src_stage, dst_stage, {}, {}, buffer_barriers, image_barriers);
            }
        }
    }
}

}  // namespace VulkanEngine::RenderGraph
