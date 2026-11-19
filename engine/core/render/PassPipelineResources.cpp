module;

#include <logging/logging_macros.hpp>

module VulkanEngine.PassPipelineResources;

import std;
import std.compat;

import logiface;

import vulkan;

import VulkanEngine.DescriptorDecl;
import VulkanEngine.PipelinePass;
import VulkanEngine.RenderGraph;

namespace VulkanEngine::RenderPipeline {

PassPipelineResources::~PassPipelineResources() = default;

bool PassPipelineResources::Build(const vk::raii::Device& device,
                                  const VulkanEngine::PipelinePass::PassPipelineRequest& request,
                                  std::span<const vk::DescriptorSetLayout> engine_sets,
                                  std::span<const VulkanEngine::Render::DescriptorDecl> bindings,
                                  std::uint32_t assignment_count,
                                  std::uint32_t frames_in_flight,
                                  std::string_view pass_name) {
    const std::vector<VulkanEngine::Render::DescriptorDecl> decls(bindings.begin(), bindings.end());

    // The composer is the only validator of an application declaration: it rejects the reserved engine sets and a zero descriptor count before any Vulkan object exists.
    VulkanEngine::Render::PipelineLayoutComposer composer;
    if (!composer.AddAppBindings(decls).has_value()) {
        LOGIFACE_LOG(error, "RenderPipeline: pass '" + std::string(pass_name) +
                                "' has invalid descriptor declarations");
        return false;
    }

    frames_in_flight_ = std::max<std::uint32_t>(frames_in_flight, 1);

    for (const auto& spec : VulkanEngine::Render::BuildSetLayoutSpecs(
             decls, VulkanEngine::Render::BindingCountPolicy::Declared)) {
        vk::DescriptorSetLayoutBindingFlagsCreateInfo flags_info{};
        flags_info.bindingCount = static_cast<std::uint32_t>(spec.flags.size());
        flags_info.pBindingFlags = spec.flags.data();
        vk::DescriptorSetLayoutCreateInfo layout_info{};
        layout_info.pNext = &flags_info;
        layout_info.bindingCount = static_cast<std::uint32_t>(spec.bindings.size());
        layout_info.pBindings = spec.bindings.data();
        app_set_layouts_.emplace_back(device, layout_info);
        app_set_numbers_.push_back(spec.set);
    }

    std::vector<vk::DescriptorSetLayout> set_layouts(engine_sets.begin(), engine_sets.end());
    for (auto& layout : app_set_layouts_) {
        set_layouts.push_back(*layout);
    }

    std::vector<vk::PushConstantRange> push_ranges;
    if (request.push_constant_size > 0 && request.push_constant_stages != vk::ShaderStageFlags{}) {
        push_ranges.push_back(vk::PushConstantRange{
            request.push_constant_stages, 0, request.push_constant_size});
    }

    vk::PipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.setLayoutCount = static_cast<std::uint32_t>(set_layouts.size());
    pipeline_layout_info.pSetLayouts = set_layouts.data();
    pipeline_layout_info.pushConstantRangeCount = static_cast<std::uint32_t>(push_ranges.size());
    pipeline_layout_info.pPushConstantRanges = push_ranges.data();
    layout_ = std::make_unique<vk::raii::PipelineLayout>(device, pipeline_layout_info);
    if (static_cast<vk::PipelineLayout>(**layout_) == nullptr) {
        LOGIFACE_LOG(error, "RenderPipeline: failed to create layout for pass '" +
                                std::string(pass_name) + "'");
        return false;
    }

    // One descriptor set per declared app set per frames-in-flight slot.
    // A frame rewrites only its own slot's sets before recording, so a set is never updated while an in-flight frame references it.
    if (!app_set_layouts_.empty()) {
        const std::uint32_t group_count = static_cast<std::uint32_t>(app_set_layouts_.size());

        std::map<std::uint32_t, std::uint32_t> size_by_type;
        for (const auto& decl : decls) {
            size_by_type[static_cast<std::uint32_t>(decl.descriptor_type)] += decl.count * frames_in_flight_;
        }
        std::vector<vk::DescriptorPoolSize> pool_sizes;
        pool_sizes.reserve(size_by_type.size());
        for (const auto& [type, count] : size_by_type) {
            pool_sizes.push_back(vk::DescriptorPoolSize{
                static_cast<vk::DescriptorType>(type), count});
        }
        vk::DescriptorPoolCreateInfo pool_info{};
        // raii descriptor sets free themselves, which requires the pool flag.
        pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        pool_info.maxSets = frames_in_flight_ * group_count;
        pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        app_pool_ = std::make_unique<vk::raii::DescriptorPool>(device, pool_info);

        app_sets_.resize(frames_in_flight_);
        app_set_handles_.resize(frames_in_flight_);
        last_written_.resize(frames_in_flight_);
        for (std::uint32_t slot = 0; slot < frames_in_flight_; ++slot) {
            std::vector<vk::DescriptorSetLayout> app_layouts;
            app_layouts.reserve(app_set_layouts_.size());
            for (const auto& app_layout : app_set_layouts_) {
                app_layouts.push_back(*app_layout);
            }
            vk::DescriptorSetAllocateInfo allocate_info{};
            allocate_info.descriptorPool = *app_pool_;
            allocate_info.descriptorSetCount = static_cast<std::uint32_t>(app_layouts.size());
            allocate_info.pSetLayouts = app_layouts.data();
            app_sets_[slot] = device.allocateDescriptorSets(allocate_info);
            app_set_handles_[slot].clear();
            app_set_handles_[slot].reserve(app_sets_[slot].size());
            for (const auto& set : app_sets_[slot]) {
                app_set_handles_[slot].push_back(*set);
            }
            last_written_[slot].assign(assignment_count, DescriptorBindingState{});
        }
    }

    return true;
}

vk::PipelineLayout PassPipelineResources::Layout() const {
    return layout_ == nullptr ? nullptr : **layout_;
}

std::span<const vk::DescriptorSet> PassPipelineResources::UpdateForFrame(
    const vk::raii::Device& device,
    std::uint32_t ring_index,
    const VulkanEngine::RenderGraph::CompiledRenderGraph& graph,
    const VulkanEngine::PipelinePass::IResourceLookup& lookup,
    std::span<const VulkanEngine::PipelinePass::BindingAssignment> assignments,
    std::span<const VulkanEngine::Render::DescriptorDecl> bindings,
    vk::Sampler default_sampler) {
    if (app_sets_.empty()) {
        return {};
    }
    const std::uint32_t fif = ring_index % frames_in_flight_;
    if (fif >= app_set_handles_.size()) {
        return {};
    }

    std::vector<vk::WriteDescriptorSet> writes;
    std::vector<vk::DescriptorImageInfo> image_infos(assignments.size());
    std::vector<vk::DescriptorBufferInfo> buffer_infos(assignments.size());
    auto& last_written = last_written_[fif];

    for (std::size_t i = 0; i < assignments.size(); ++i) {
        const auto& assignment = assignments[i];
        const auto decl_it = std::ranges::find_if(bindings, [&](const auto& candidate) {
            return candidate.set == assignment.set && candidate.binding == assignment.binding;
        });
        if (decl_it == bindings.end()) {
            continue;
        }
        const auto set_it = std::ranges::find(app_set_numbers_, assignment.set);
        if (set_it == app_set_numbers_.end()) {
            continue;
        }
        const auto set_index = static_cast<std::size_t>(
            std::distance(app_set_numbers_.begin(), set_it));
        if (set_index >= app_set_handles_[fif].size()) {
            continue;
        }
        // The compiled graph carries the name each resource was declared with, so the lookup needs no separate name table.
        if (assignment.resource.index >= graph.resource_info.size()) {
            continue;
        }
        const auto resolved = lookup.Find(graph.resource_info[assignment.resource.index].name);
        if (!resolved.has_value()) {
            continue;
        }
        const auto& resource = *resolved;

        DescriptorBindingState key{};
        vk::WriteDescriptorSet write{};
        write.dstSet = app_set_handles_[fif][set_index];
        write.dstBinding = assignment.binding;
        write.descriptorCount = 1;
        write.descriptorType = decl_it->descriptor_type;

        using vk::DescriptorType;
        switch (decl_it->descriptor_type) {
            case DescriptorType::eCombinedImageSampler:
            case DescriptorType::eSampledImage: {
                key.view = resource.AsImageView();
                key.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
                key.sampler = decl_it->descriptor_type == DescriptorType::eCombinedImageSampler
                                  ? default_sampler
                                  : nullptr;
                image_infos[i] = vk::DescriptorImageInfo{key.sampler, key.view, key.layout};
                write.pImageInfo = &image_infos[i];
                break;
            }
            case DescriptorType::eStorageImage: {
                key.view = resource.AsImageView();
                key.layout = vk::ImageLayout::eGeneral;
                image_infos[i] = vk::DescriptorImageInfo{nullptr, key.view, key.layout};
                write.pImageInfo = &image_infos[i];
                break;
            }
            case DescriptorType::eStorageBuffer:
            case DescriptorType::eUniformBuffer: {
                key.buffer = resource.AsBuffer();
                key.offset = resource.GetOffset();
                key.size = resource.GetSize();
                buffer_infos[i] = vk::DescriptorBufferInfo{key.buffer, key.offset, key.size};
                write.pBufferInfo = &buffer_infos[i];
                break;
            }
            default:
                continue;
        }

        key.valid = true;
        auto& last = last_written[i];
        const bool changed =
            !last.valid || last.view != key.view || last.buffer != key.buffer ||
            last.sampler != key.sampler || last.offset != key.offset ||
            last.size != key.size || last.layout != key.layout;
        if (changed) {
            writes.push_back(write);
            last = key;
        }
    }

    if (!writes.empty()) {
        device.updateDescriptorSets(writes, {});
    }
    return app_set_handles_[fif];
}

} // namespace VulkanEngine::RenderPipeline
