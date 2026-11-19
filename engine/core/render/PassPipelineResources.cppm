module;

#include <logging/logging_macros.hpp>

export module VulkanEngine.PassPipelineResources;

import std;
import std.compat;

import logiface;

import vulkan;

import VulkanEngine.DescriptorDecl;
import VulkanEngine.PipelinePass;
import VulkanEngine.RenderGraph;

export namespace VulkanEngine::RenderPipeline {

// The Vulkan side of one declared pass holds its application set layouts, pipeline layout, descriptor pool, per-frame-in-flight descriptor sets and the write cache that keeps a steady-state frame from rewriting unchanged bindings.
// The declaration itself stays in the render pipeline's pass model and is passed in per call, so there is exactly one stored copy.
class PassPipelineResources {
public:
    PassPipelineResources() = default;
    ~PassPipelineResources();
    PassPipelineResources(const PassPipelineResources&) = delete;
    PassPipelineResources& operator=(const PassPipelineResources&) = delete;
    PassPipelineResources(PassPipelineResources&&) = default;
    PassPipelineResources& operator=(PassPipelineResources&&) = default;

    // Creates the pipeline layout, one descriptor set per app set per frames-in-flight slot, and sizes the per-slot write cache.
    // `assignment_count` is the pass's BindingAssignment count, not its declaration count: the cache is indexed by assignment, and a pass may bind the same (set, binding) twice.
    // A declaration the composer rejects (reserved engine set, zero count) makes Build return false before anything is created, so the caller logs and skips the pass instead of letting vk::raii throw.
    bool Build(const vk::raii::Device& device,
               const VulkanEngine::PipelinePass::PassPipelineRequest& request,
               std::span<const vk::DescriptorSetLayout> engine_sets,
               std::span<const VulkanEngine::Render::DescriptorDecl> bindings,
               std::uint32_t assignment_count,
               std::uint32_t frames_in_flight,
               std::string_view pass_name);

    [[nodiscard]] vk::PipelineLayout Layout() const;

    // Writes the bindings whose resolved value changed since this slot last wrote them, and returns the sets to bind.
    // Returns an empty span when the pass declared no application sets.
    [[nodiscard]] std::span<const vk::DescriptorSet> UpdateForFrame(
        const vk::raii::Device& device,
        std::uint32_t ring_index,
        const VulkanEngine::RenderGraph::CompiledRenderGraph& graph,
        const VulkanEngine::PipelinePass::IResourceLookup& lookup,
        std::span<const VulkanEngine::PipelinePass::BindingAssignment> assignments,
        std::span<const VulkanEngine::Render::DescriptorDecl> bindings,
        vk::Sampler default_sampler);

private:
    // What a slot's binding was last written with, so an unchanged binding costs no descriptor update.
    struct DescriptorBindingState {
        vk::ImageView view = nullptr;
        vk::Buffer buffer = nullptr;
        vk::Sampler sampler = nullptr;
        vk::DeviceSize offset = 0;
        vk::DeviceSize size = 0;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
        bool valid = false;
    };

    // Descriptor set layouts must outlive the pipeline layout that references them, so one class owns both.
    std::vector<vk::raii::DescriptorSetLayout> app_set_layouts_{};
    std::unique_ptr<vk::raii::PipelineLayout> layout_{};
    // One descriptor set per declared app set per frames-in-flight slot.
    // A frame rewrites only its own slot's sets before recording, so a set is never updated while an in-flight frame references it.
    std::unique_ptr<vk::raii::DescriptorPool> app_pool_{};
    std::vector<std::vector<vk::raii::DescriptorSet>> app_sets_{};
    std::vector<std::vector<vk::DescriptorSet>> app_set_handles_{};
    std::vector<std::uint32_t> app_set_numbers_{};
    std::vector<std::vector<DescriptorBindingState>> last_written_{};
    std::uint32_t frames_in_flight_ = 1;
};

} // namespace VulkanEngine::RenderPipeline
