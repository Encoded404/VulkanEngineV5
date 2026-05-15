module;

#include <algorithm>
#include <cstdint>
#include <queue>
#include <ranges>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "logging/logging.hpp"

module VulkanEngine.RenderGraph;

namespace VulkanEngine::RenderGraph {

namespace {

bool ContainsResource(const std::vector<ResourceHandle>& handles, ResourceHandle value) {
    return std::ranges::find(handles, value) != handles.end();
}

bool ContainsDependency(const std::vector<std::pair<PassHandle, PassHandle>>& deps,
                        const std::pair<PassHandle, PassHandle>& value) {
    return std::ranges::find(deps, value) != deps.end();
}

bool IsResourceStateCompatible(ResourceKind kind, const ResourceState& state) {
    if (kind == ResourceKind::Image) {
        return state.has_image_layout;
    }
    return !state.has_image_layout;
}

vk::PipelineStageFlags IntentToPipelineStage(PipelineStageIntent intent, AccessIntent access) {
    switch (intent) {
        case PipelineStageIntent::None:
        case PipelineStageIntent::TopOfPipe:
            return vk::PipelineStageFlagBits::eTopOfPipe;
        case PipelineStageIntent::BottomOfPipe:
            return vk::PipelineStageFlagBits::eBottomOfPipe;
        case PipelineStageIntent::Transfer:
            return vk::PipelineStageFlagBits::eTransfer;
        case PipelineStageIntent::ColorAttachment:
            return vk::PipelineStageFlagBits::eColorAttachmentOutput;
        case PipelineStageIntent::DepthAttachment:
            return vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
        case PipelineStageIntent::FragmentShader:
            return access == AccessIntent::Write
                       ? vk::PipelineStageFlagBits::eColorAttachmentOutput
                       : vk::PipelineStageFlagBits::eFragmentShader;
        case PipelineStageIntent::ComputeShader:
            return vk::PipelineStageFlagBits::eComputeShader;
        case PipelineStageIntent::Present:
            return vk::PipelineStageFlagBits::eBottomOfPipe;
        default:
            return vk::PipelineStageFlagBits::eTopOfPipe;
    }
}

vk::AccessFlags IntentToAccessFlags(PipelineStageIntent stage, AccessIntent access) {
    if (access == AccessIntent::None) {
        return {};
    }

    switch (stage) {
        case PipelineStageIntent::Transfer:
            return access == AccessIntent::Write
                       ? vk::AccessFlagBits::eTransferWrite
                       : vk::AccessFlagBits::eTransferRead;
        case PipelineStageIntent::ColorAttachment:
            return access == AccessIntent::Write
                       ? vk::AccessFlagBits::eColorAttachmentWrite
                       : vk::AccessFlagBits::eColorAttachmentRead;
        case PipelineStageIntent::DepthAttachment:
            return access == AccessIntent::Write
                       ? vk::AccessFlagBits::eDepthStencilAttachmentWrite
                       : vk::AccessFlagBits::eDepthStencilAttachmentRead;
        case PipelineStageIntent::FragmentShader:
            return access == AccessIntent::Write
                       ? vk::AccessFlagBits::eColorAttachmentWrite
                       : vk::AccessFlagBits::eShaderRead;
        case PipelineStageIntent::ComputeShader:
            return access == AccessIntent::Write
                       ? vk::AccessFlagBits::eShaderWrite
                       : vk::AccessFlagBits::eShaderRead;
        /*case PipelineStageIntent::Present:
            return {};*/
        default:
            return {};
    }
}

vk::ImageLayout IntentToImageLayout(ImageLayoutIntent intent) {
    switch (intent) {
        case ImageLayoutIntent::Undefined:
            return vk::ImageLayout::eUndefined;
        case ImageLayoutIntent::General:
            return vk::ImageLayout::eGeneral;
        case ImageLayoutIntent::ColorAttachment:
            return vk::ImageLayout::eColorAttachmentOptimal;
        case ImageLayoutIntent::DepthAttachment:
            return vk::ImageLayout::eDepthAttachmentOptimal;
        case ImageLayoutIntent::DepthReadOnly:
            return vk::ImageLayout::eDepthReadOnlyOptimal;
        case ImageLayoutIntent::ShaderReadOnly:
            return vk::ImageLayout::eShaderReadOnlyOptimal;
        case ImageLayoutIntent::TransferSource:
            return vk::ImageLayout::eTransferSrcOptimal;
        case ImageLayoutIntent::TransferDestination:
            return vk::ImageLayout::eTransferDstOptimal;
        case ImageLayoutIntent::Present:
            return vk::ImageLayout::ePresentSrcKHR;
        default:
            return vk::ImageLayout::eUndefined;
    }
}

vk::ImageAspectFlags FormatToAspectFlags(vk::Format format) {
    switch (format) {
        case vk::Format::eD16Unorm:
        case vk::Format::eD32Sfloat:
        case vk::Format::eD16UnormS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD32SfloatS8Uint:
            return vk::ImageAspectFlagBits::eDepth;
        default:
            return vk::ImageAspectFlagBits::eColor;
    }
}

bool StatesEqual(const ResourceState& a, const ResourceState& b) {
    if (a.has_image_layout != b.has_image_layout) return false;
    if (a.has_image_layout && a.layout != b.layout) return false;
    return a.stage == b.stage && a.access == b.access;
}

}  // namespace

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
        // Compute and record pre-pass barriers dynamically
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

            // Fix up stage flags for TOP_OF_PIPE / BOTTOM_OF_PIPE
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
            if (pass.attachment_setup->auto_begin_rendering) { //NOLINT(bugprone-unchecked-optional-access)
                const auto& setup = *pass.attachment_setup; //NOLINT(bugprone-unchecked-optional-access)

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
                    depth_attachment->imageView = setup.depth_attachment->image_view; //NOLINT(bugprone-unchecked-optional-access)
                    depth_attachment->imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
                    depth_attachment->loadOp = setup.depth_attachment->load_op; //NOLINT(bugprone-unchecked-optional-access)
                    depth_attachment->storeOp = setup.depth_attachment->store_op; //NOLINT(bugprone-unchecked-optional-access)
                    if (setup.depth_attachment->load_op == vk::AttachmentLoadOp::eClear) { //NOLINT(bugprone-unchecked-optional-access)
                        depth_attachment->clearValue = vk::ClearValue(setup.depth_attachment->clear_depth); //NOLINT(bugprone-unchecked-optional-access)
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

        if (pass.attachment_setup.has_value() && pass.attachment_setup->auto_begin_rendering) { //NOLINT(bugprone-unchecked-optional-access)
            command_buffer.endRendering();
        }

        // Compute and record post-pass barriers dynamically
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

ResourceHandle RenderGraphBuilder::CreateTransientResource(std::string name, ResourceKind kind) {
    const uint32_t index = static_cast<uint32_t>(resources_.size());
    resources_.push_back(ResourceNode{
        .name = std::move(name),
        .kind = kind,
        .generation = 1,
        .imported = false,
        .transient = true,
    });

    return ResourceHandle{.index = index, .generation = resources_.back().generation};
}

ResourceHandle RenderGraphBuilder::ImportResource(std::string name, ResourceKind kind) {
    LOGIFACE_LOG(trace, "Importing resource '" + name + "'");

    const uint32_t index = static_cast<uint32_t>(resources_.size());
    resources_.push_back(ResourceNode{
        .name = std::move(name),
        .kind = kind,
        .generation = 1,
        .imported = true,
        .transient = false,
    });

    return ResourceHandle{.index = index, .generation = resources_.back().generation};
}

bool RenderGraphBuilder::SetTransientImageInfo(ResourceHandle resource, TransientImageInfo info) {
    if (!IsValidResourceHandle(resource)) {
        return false;
    }

    auto& resource_node = resources_[resource.index];
    if (!resource_node.transient || resource_node.kind != ResourceKind::Image) {
        return false;
    }

    resource_node.image_info = info;
    return true;
}

bool RenderGraphBuilder::SetTransientBufferInfo(ResourceHandle resource, TransientBufferInfo info) {
    if (!IsValidResourceHandle(resource)) {
        return false;
    }

    auto& resource_node = resources_[resource.index];
    if (!resource_node.transient || resource_node.kind != ResourceKind::Buffer) {
        return false;
    }

    resource_node.buffer_info = info;
    return true;
}

bool RenderGraphBuilder::SetInitialState(ResourceHandle resource, ResourceState state) {
    if (!IsValidResourceHandle(resource)) {
        return false;
    }

    auto& resource_node = resources_[resource.index];
    if (!IsResourceStateCompatible(resource_node.kind, state)) {
        return false;
    }

    resource_node.initial_state = state;
    resource_node.has_initial_state = true;
    return true;
}

bool RenderGraphBuilder::SetFinalState(ResourceHandle resource, ResourceState state) {
    if (!IsValidResourceHandle(resource)) {
        return false;
    }

    auto& resource_node = resources_[resource.index];
    if (!IsResourceStateCompatible(resource_node.kind, state)) {
        return false;
    }

    resource_node.final_state = state;
    resource_node.has_final_state = true;
    return true;
}

PassHandle RenderGraphBuilder::AddPass(std::string name, QueueType queue, bool enabled, PassExecutionCallback execute) {
    LOGIFACE_LOG(trace, "Adding pass '" + name + "' with queue type " + std::to_string(static_cast<int>(queue)));

    const uint32_t index = static_cast<uint32_t>(passes_.size());
    passes_.push_back(PassNode{
        .name = std::move(name),
        .queue = queue,
        .generation = 1,
        .enabled = enabled,
        .execute = std::move(execute),
    });

    return PassHandle{.index = index, .generation = passes_.back().generation};
}

bool RenderGraphBuilder::AddRead(PassHandle pass, ResourceHandle resource) {
    if (!IsValidPassHandle(pass) || !IsValidResourceHandle(resource)) {
        return false;
    }

    auto& pass_node = passes_[pass.index];
    if (!ContainsResource(pass_node.reads, resource)) {
        pass_node.reads.push_back(resource);
    }

    return true;
}

bool RenderGraphBuilder::AddWrite(PassHandle pass, ResourceHandle resource) {
    LOGIFACE_LOG(trace, "Adding write to pass '" + passes_[pass.index].name + "' for resource '" + resources_[resource.index].name + "'");

    if (!IsValidPassHandle(pass) || !IsValidResourceHandle(resource)) {
        return false;
    }

    auto& pass_node = passes_[pass.index];
    if (!ContainsResource(pass_node.writes, resource)) {
        pass_node.writes.push_back(resource);
    }

    LOGIFACE_LOG(trace, "returning from AddWrite successfully");
    return true;
}

bool RenderGraphBuilder::AddDependency(PassHandle before, PassHandle after) {
    if (!IsValidPassHandle(before) || !IsValidPassHandle(after) || before == after) {
        return false;
    }

    const std::pair<PassHandle, PassHandle> dependency{before, after};
    if (!ContainsDependency(explicit_dependencies_, dependency)) {
        explicit_dependencies_.emplace_back(before, after);
    }

    return true;
}

bool RenderGraphBuilder::SetPassAttachments(PassHandle pass, PassAttachmentSetup setup) {
    if (!IsValidPassHandle(pass)) {
        return false;
    }

    passes_[pass.index].attachment_setup = std::move(setup);
    return true;
}

CompiledRenderGraph RenderGraphBuilder::Compile() const {
    CompiledRenderGraph result{};

    auto emit_diagnostic = [&](DiagnosticCode code, DiagnosticSeverity severity, std::string message) {
        result.diagnostics.push_back(CompileDiagnostic{
            .code = code,
            .severity = severity,
            .message = std::move(message),
        });
    };

    if (passes_.empty()) {
        result.success = true;
        emit_diagnostic(
            DiagnosticCode::EmptyGraph,
            DiagnosticSeverity::Info,
            "Render graph has no passes; compile produced an empty execution plan.");
        return result;
    }

    for (const auto& resource : resources_) {
        if (resource.has_initial_state && !IsResourceStateCompatible(resource.kind, resource.initial_state)) {
            emit_diagnostic(
                DiagnosticCode::InvalidInitialState,
                DiagnosticSeverity::Error,
                "Render graph compile failed: resource '" + resource.name + "' has an incompatible initial state.");
        }
        if (resource.has_final_state && !IsResourceStateCompatible(resource.kind, resource.final_state)) {
            emit_diagnostic(
                DiagnosticCode::InvalidFinalState,
                DiagnosticSeverity::Error,
                "Render graph compile failed: resource '" + resource.name + "' has an incompatible final state.");
        }
    }

    const size_t pass_count = passes_.size();
    std::vector<std::unordered_set<uint32_t>> edges(pass_count);
    std::vector<uint32_t> indegree(pass_count, 0);

    auto add_edge = [&](uint32_t from, uint32_t to) {
        if (from == to) {
            return;
        }
        if (edges[from].insert(to).second) {
            ++indegree[to];
        }
    };

    for (const auto& [before, after] : explicit_dependencies_) {
        if (!IsValidPassHandle(before) || !IsValidPassHandle(after)) {
            emit_diagnostic(
                DiagnosticCode::InvalidExplicitDependency,
                DiagnosticSeverity::Error,
                "Render graph contains an invalid explicit pass dependency.");
            continue;
        }

        if (!passes_[before.index].enabled || !passes_[after.index].enabled) {
            continue;
        }

        add_edge(before.index, after.index);
    }

    struct ResourceTracker {
        int32_t last_writer = -1;
        std::unordered_set<uint32_t> readers{};
    };

    std::vector<ResourceTracker> resource_trackers(resources_.size());

    for (uint32_t pass_index = 0; pass_index < pass_count; ++pass_index) {
        const auto& pass = passes_[pass_index];
        if (!pass.enabled) {
            continue;
        }

        for (const auto& read : pass.reads) {
            if (!IsValidResourceHandle(read)) {
                emit_diagnostic(
                    DiagnosticCode::InvalidReadHandle,
                    DiagnosticSeverity::Error,
                    "Pass '" + pass.name + "' references an invalid read resource handle.");
                continue;
            }

            auto& tracker = resource_trackers[read.index];
            if (tracker.last_writer >= 0) {
                add_edge(static_cast<uint32_t>(tracker.last_writer), pass_index);
            }
            tracker.readers.insert(pass_index);
        }

        for (const auto& write : pass.writes) {
            if (!IsValidResourceHandle(write)) {
                emit_diagnostic(
                    DiagnosticCode::InvalidWriteHandle,
                    DiagnosticSeverity::Error,
                    "Pass '" + pass.name + "' references an invalid write resource handle.");
                continue;
            }

            auto& tracker = resource_trackers[write.index];
            if (tracker.last_writer >= 0) {
                add_edge(static_cast<uint32_t>(tracker.last_writer), pass_index);
            }
            for (const uint32_t reader_index : tracker.readers) {
                add_edge(reader_index, pass_index);
            }
            tracker.readers.clear();
            tracker.last_writer = static_cast<int32_t>(pass_index);
        }
    }

    std::queue<uint32_t> ready{};
    for (uint32_t pass_index = 0; pass_index < pass_count; ++pass_index) {
        if (!passes_[pass_index].enabled) {
            continue;
        }
        if (indegree[pass_index] == 0) {
            ready.push(pass_index);
        }
    }

    std::vector<uint32_t> sorted_indices{};
    while (!ready.empty()) {
        const uint32_t current = ready.front();
        ready.pop();
        sorted_indices.push_back(current);

        for (const uint32_t dependent : edges[current]) {
            if (--indegree[dependent] == 0) {
                ready.push(dependent);
            }
        }
    }

    const uint32_t enabled_count = static_cast<uint32_t>(std::ranges::count_if(passes_, [](const PassNode& pass) {
        return pass.enabled;
    }));

    if (sorted_indices.size() != enabled_count) {
        emit_diagnostic(
            DiagnosticCode::CycleDetected,
            DiagnosticSeverity::Error,
            "Render graph compile failed: pass dependency cycle detected.");
        result.success = false;
        return result;
    }

    struct ResourcePassState {
        ResourceState state{};
        bool has_state = false;
    };

    std::vector<ResourcePassState> resource_states(resources_.size());

    for (uint32_t resource_index = 0; resource_index < resources_.size(); ++resource_index) {
        const auto& resource = resources_[resource_index];
        if (resource.has_initial_state) {
            resource_states[resource_index] = {resource.initial_state, true};
        }
    }

    std::vector<CompiledPass> compiled_passes;
    compiled_passes.reserve(sorted_indices.size());

    auto make_transition = [&](uint32_t res_idx, const ResourceState& target) -> ResourceTransition {
        ResourceTransition t{};
        t.resource_index = res_idx;
        t.target_state = target;
        return t;
    };

    for (const unsigned int pass_index : sorted_indices) {
        const auto& pass = passes_[pass_index];
        const PassHandle handle{.index = pass_index, .generation = pass.generation};

        CompiledPass compiled_pass{};
        compiled_pass.handle = handle;
        compiled_pass.name = pass.name;
        compiled_pass.queue = pass.queue;
        compiled_pass.execute = pass.execute;
        compiled_pass.attachment_setup = pass.attachment_setup;

        for (const auto& write : pass.writes) {
            if (!IsValidResourceHandle(write)) continue;
            const auto& resource = resources_[write.index];
            if (resource.kind != ResourceKind::Image) continue;

            auto& current_state = resource_states[write.index];

            ResourceState target_state;
            if (pass.attachment_setup && pass.attachment_setup->depth_attachment &&
                       pass.attachment_setup->depth_attachment->resource.index == write.index) {
                target_state = ResourceState::ImageState(
                    PipelineStageIntent::DepthAttachment, AccessIntent::Write,
                    QueueType::Graphics, ImageLayoutIntent::DepthAttachment);
            } else {
                target_state = ResourceState::ImageState(
                    PipelineStageIntent::ColorAttachment, AccessIntent::Write,
                    QueueType::Graphics, ImageLayoutIntent::ColorAttachment);
            }

            if (!current_state.has_state) {
                const ResourceState undefined_state = ResourceState::ImageState(
                    PipelineStageIntent::TopOfPipe, AccessIntent::None,
                    QueueType::Graphics, ImageLayoutIntent::Undefined);
                if (!StatesEqual(undefined_state, target_state)) {
                    compiled_pass.pre_pass_transitions.push_back(
                        make_transition(write.index, target_state));
                }
            } else if (!StatesEqual(current_state.state, target_state)) {
                compiled_pass.pre_pass_transitions.push_back(
                    make_transition(write.index, target_state));
            }

            current_state.state = target_state;
            current_state.has_state = true;
        }

        for (const auto& read : pass.reads) {
            if (!IsValidResourceHandle(read)) continue;
            const auto& resource = resources_[read.index];
            if (resource.kind != ResourceKind::Image) continue;

            auto& current_state = resource_states[read.index];

            ResourceState target_state;
            if (resource.image_info && (resource.image_info->usage & vk::ImageUsageFlagBits::eDepthStencilAttachment)) {
                target_state = ResourceState::ImageState(
                    PipelineStageIntent::DepthAttachment, AccessIntent::Read,
                    QueueType::Graphics, ImageLayoutIntent::DepthAttachment);
            } else {
                target_state = ResourceState::ImageState(
                    PipelineStageIntent::FragmentShader, AccessIntent::Read,
                    QueueType::Graphics, ImageLayoutIntent::ShaderReadOnly);
            }

            if (!current_state.has_state) {
                const ResourceState undefined_state = ResourceState::ImageState(
                    PipelineStageIntent::TopOfPipe, AccessIntent::None,
                    QueueType::Graphics, ImageLayoutIntent::Undefined);
                if (!StatesEqual(undefined_state, target_state)) {
                    compiled_pass.pre_pass_transitions.push_back(
                        make_transition(read.index, target_state));
                }
            } else if (!StatesEqual(current_state.state, target_state)) {
                compiled_pass.pre_pass_transitions.push_back(
                    make_transition(read.index, target_state));
            }

            current_state.state = target_state;
            current_state.has_state = true;
        }

        compiled_passes.push_back(std::move(compiled_pass));
    }

    for (size_t ordered_index = 0; ordered_index < sorted_indices.size(); ++ordered_index) {
        const uint32_t pass_index = sorted_indices[ordered_index];
        const auto& pass = passes_[pass_index];

        for (const auto& write : pass.writes) {
            if (!IsValidResourceHandle(write)) continue;
            const auto& resource = resources_[write.index];
            if (resource.kind != ResourceKind::Image || !resource.has_final_state) continue;

            const int32_t current_oi = static_cast<int32_t>(ordered_index);
            int32_t last_usage = -1;
            for (int32_t oi = static_cast<int32_t>(sorted_indices.size()) - 1; oi > current_oi; --oi) {
                const auto& p = passes_[sorted_indices[static_cast<size_t>(oi)]];
                const ResourceHandle h{.index = write.index, .generation = resource.generation};
                if (ContainsResource(p.writes, h) || ContainsResource(p.reads, h)) {
                    last_usage = oi;
                    break;
                }
            }

            if (last_usage < 0) {
                auto& current_state = resource_states[write.index];
                if (current_state.has_state && !StatesEqual(current_state.state, resource.final_state)) {
                    compiled_passes[ordered_index].post_pass_transitions.push_back(
                        make_transition(write.index, resource.final_state));
                }
            }
        }
    }

    result.passes = std::move(compiled_passes);

    result.resource_lifetimes.reserve(resources_.size());
    result.resource_info.reserve(resources_.size());
    result.initial_states.resize(resources_.size());
    result.has_initial_state.resize(resources_.size(), false);
    result.resource_images.resize(resources_.size());
    result.resource_formats.resize(resources_.size(), vk::Format::eUndefined);

    for (uint32_t resource_index = 0; resource_index < resources_.size(); ++resource_index) {
        const auto& resource = resources_[resource_index];

        int32_t first = -1;
        int32_t last = -1;
        for (size_t ordered_index = 0; ordered_index < sorted_indices.size(); ++ordered_index) {
            const auto pass_index = sorted_indices[ordered_index];
            const auto& pass = passes_[pass_index];
            const ResourceHandle handle{.index = resource_index, .generation = resource.generation};
            if (ContainsResource(pass.reads, handle) || ContainsResource(pass.writes, handle)) {
                if (first < 0) {
                    first = static_cast<int32_t>(ordered_index);
                }
                last = static_cast<int32_t>(ordered_index);
            }
        }

        result.resource_lifetimes.push_back(ResourceLifetime{
            .handle = ResourceHandle{.index = resource_index, .generation = resource.generation},
            .name = resource.name,
            .imported = resource.imported,
            .transient = resource.transient,
            .first_pass = first,
            .last_pass = last,
        });

        result.resource_info.push_back(ResourceInfo{
            .name = resource.name,
            .kind = resource.kind,
            .imported = resource.imported,
            .image_info = resource.image_info,
            .buffer_info = resource.buffer_info,
        });

        if (resource.image_info) {
            result.resource_formats[resource_index] = resource.image_info->format;
        }

        if (resource.has_initial_state) {
            result.initial_states[resource_index] = resource.initial_state;
            result.has_initial_state[resource_index] = true;
        }
    }

    const bool has_errors = std::ranges::any_of(result.diagnostics, [](const CompileDiagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });

    result.success = !has_errors;
    if (result.success) {
        emit_diagnostic(
            DiagnosticCode::CompileSuccess,
            DiagnosticSeverity::Info,
            "Render graph compile succeeded.");
    }

    return result;
}

bool RenderGraphBuilder::IsValidResourceHandle(ResourceHandle handle) const {
    if (!handle.IsValid()) {
        return false;
    }

    if (handle.index >= resources_.size()) {
        return false;
    }

    return resources_[handle.index].generation == handle.generation;
}

bool RenderGraphBuilder::IsValidPassHandle(PassHandle handle) const {
    if (!handle.IsValid()) {
        return false;
    }

    if (handle.index >= passes_.size()) {
        return false;
    }

    return passes_[handle.index].generation == handle.generation;
}

}  // namespace VulkanEngine::RenderGraph
