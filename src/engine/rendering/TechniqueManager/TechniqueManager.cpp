module;


module VulkanEngine.TechniqueManager;

import std;
import std.compat;

import std;

import vulkan_hpp;

import VulkanEngine.StandardMeshPipeline;

namespace {
    std::vector<std::uint32_t> ResolveSpv(const std::vector<std::uint32_t>& override_spv,
                                              const std::vector<std::uint32_t>& default_spv) {
        if (!override_spv.empty()) return override_spv;
        return default_spv;
    }
}

namespace VulkanEngine::TechniqueManager {

TechniqueManager::~TechniqueManager() {
    Shutdown();
}

uint16_t TechniqueManager::RegisterTechnique(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                               const std::vector<std::uint32_t>& vert_spv,
                                               const std::vector<std::uint32_t>& frag_spv,
                                               const VulkanEngine::StandardMeshPipeline::PipelineConfig& config,
                                               vk::DescriptorSetLayout* bindless_layout,
                                               vk::DescriptorSetLayout* object_data_layout,
                                               vk::DescriptorSetLayout* raw_vertex_layout,
                                               vk::DescriptorSetLayout* indirection_layout) {
    if (!has_defaults_) {
        default_vert_spv_ = vert_spv;
        default_frag_spv_ = frag_spv;
        has_defaults_ = true;
    }

    const uint16_t id = static_cast<uint16_t>(techniques_.size());

    auto graphics_pipeline = std::make_unique<VulkanEngine::StandardMeshPipeline::GraphicsPipeline>();
    graphics_pipeline->Initialize(bootstrap, vert_spv, frag_spv, config, bindless_layout, object_data_layout, raw_vertex_layout, indirection_layout);

    techniques_.push_back(Technique{std::move(graphics_pipeline)});

    vk::Pipeline pipe = techniques_.back().graphics_pipeline->GetPipeline()
        ? static_cast<vk::Pipeline>(**techniques_.back().graphics_pipeline->GetPipeline())
        : nullptr;
    vk::PipelineLayout layout = techniques_.back().graphics_pipeline->GetPipelineLayout()
        ? *techniques_.back().graphics_pipeline->GetPipelineLayout()
        : nullptr;
    on_technique_changed.Call(id, pipe, layout);

    return id;
}

uint16_t TechniqueManager::RegisterTechnique(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                               const ShaderOverride& override,
                                               const VulkanEngine::StandardMeshPipeline::PipelineConfig& config,
                                               vk::DescriptorSetLayout* bindless_layout,
                                               vk::DescriptorSetLayout* object_data_layout,
                                               vk::DescriptorSetLayout* raw_vertex_layout,
                                               vk::DescriptorSetLayout* indirection_layout) {
    if (!has_defaults_) {
        return UINT16_MAX;
    }

    const auto resolved_vert = ResolveSpv(override.vertex_spv, default_vert_spv_);
    const auto resolved_frag = ResolveSpv(override.fragment_spv, default_frag_spv_);

    return RegisterTechnique(bootstrap, resolved_vert, resolved_frag, config,
                              bindless_layout, object_data_layout, raw_vertex_layout, indirection_layout);
}

VulkanEngine::StandardMeshPipeline::GraphicsPipeline* TechniqueManager::GetGraphicsPipeline(uint16_t technique_id) {
    if (technique_id >= techniques_.size()) return nullptr;
    return techniques_[technique_id].graphics_pipeline.get();
}

void TechniqueManager::Shutdown() {
    techniques_.clear();
    default_vert_spv_.clear();
    default_frag_spv_.clear();
    has_defaults_ = false;
}

}

