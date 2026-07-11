module;

#include <logging/logging_macros.hpp>

module VulkanEngine.RenderGraph;

import std;
import std.compat;

import logiface;

import vulkan_hpp;

using VulkanEngine::RenderGraph::ResourceHandle;
using ReadInfo = VulkanEngine::RenderGraph::RenderGraphBuilder::ReadInfo;
using VulkanEngine::RenderGraph::PassHandle;
using VulkanEngine::RenderGraph::ResourceKind;
using VulkanEngine::RenderGraph::ResourceState;
using VulkanEngine::RenderGraph::PipelineStageIntent;
using VulkanEngine::RenderGraph::AccessIntent;
using VulkanEngine::RenderGraph::ImageLayoutIntent;



namespace VulkanEngine::RenderGraph {

// we cant use internal linkage, because we are using modules they need to be visible at module level, not only TU level.
// NOLINTBEGIN(misc-use-internal-linkage)

[[maybe_unused]] bool ContainsResource(const std::vector<ResourceHandle>& handles, ResourceHandle value) {
    return std::ranges::find(handles, value) != handles.end();
}

[[maybe_unused]] bool ContainsReadResource(const std::vector<ReadInfo>& reads, ResourceHandle value) {
    return std::ranges::find_if(reads, [value](const ReadInfo& r) { return r.resource == value; }) != reads.end();
}

[[maybe_unused]] bool ContainsDependency(const std::vector<std::pair<PassHandle, PassHandle>>& deps,
                        const std::pair<PassHandle, PassHandle>& value) {
    return std::ranges::find(deps, value) != deps.end();
}

[[maybe_unused]] bool IsResourceStateCompatible(ResourceKind kind, const ResourceState& state) {
    if (kind == ResourceKind::Image) {
        return state.has_image_layout;
    }
    return !state.has_image_layout;
}

// NOLINTEND(misc-use-internal-linkage)

ResourceHandle RenderGraphBuilder::CreateTransientResource(std::string name, ResourceKind kind) {
    const std::uint32_t index = static_cast<std::uint32_t>(resources_.size());
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

    const std::uint32_t index = static_cast<std::uint32_t>(resources_.size());
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

    const std::uint32_t index = static_cast<std::uint32_t>(passes_.size());
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
    return AddRead(pass, resource, PipelineStageIntent::FragmentShader, AccessIntent::Read);
}

bool RenderGraphBuilder::AddRead(PassHandle pass, ResourceHandle resource,
                                 PipelineStageIntent stage, AccessIntent access) {
    if (!IsValidPassHandle(pass) || !IsValidResourceHandle(resource)) {
        return false;
    }

    auto& pass_node = passes_[pass.index];
    if (!ContainsReadResource(pass_node.reads, resource)) {
        pass_node.reads.push_back(ReadInfo{resource, stage, access});
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
