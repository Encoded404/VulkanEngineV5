module;

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.hpp>

#include <logging/logging.hpp>

module VulkanBackend.RenderGraph;

namespace VulkanEngine::RenderGraph {

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
