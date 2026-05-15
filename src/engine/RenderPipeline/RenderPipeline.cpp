module;

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>

#include <vulkan/vulkan_raii.hpp>

//#include <logging/logging.hpp>

module VulkanEngine.RenderPipeline;

import VulkanEngine.RenderGraph;
import VulkanEngine.Runtime.VulkanBootstrap;

namespace VulkanEngine::RenderPipeline {

namespace {

uint32_t FindMemoryType(vk::raii::PhysicalDevice const& physical_device, uint32_t type_filter, vk::MemoryPropertyFlags properties) {
    const auto mem_properties = physical_device.getMemoryProperties();
    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("failed to find suitable memory type");
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

} // namespace

RenderPipeline::RenderPipeline() = default;
RenderPipeline::~RenderPipeline() = default;

void RenderPipeline::Initialize(VulkanEngine::Runtime::VulkanBootstrap& bootstrap) {
    bootstrap_ = &bootstrap;
    initialized_ = true;
}

void RenderPipeline::Shutdown() {
    DeallocateTransients();
    bootstrap_ = nullptr;
    initialized_ = false;
    compiled_ = false;
}

VulkanEngine::RenderGraph::ResourceHandle RenderPipeline::ImportBackbuffer() {
    backbuffer_handle_ = graph_builder_.ImportResource("swapchain-backbuffer", VulkanEngine::RenderGraph::ResourceKind::Image);
    return backbuffer_handle_;
}

VulkanEngine::RenderGraph::ResourceHandle RenderPipeline::ImportDepthBuffer() {
    depth_buffer_handle_ = graph_builder_.ImportResource("depth-buffer", VulkanEngine::RenderGraph::ResourceKind::Image);
    return depth_buffer_handle_;
}

VulkanEngine::RenderGraph::ResourceHandle RenderPipeline::CreateTransientImage(const TransientImageDesc& desc) {
    auto handle = graph_builder_.CreateTransientResource(desc.name, VulkanEngine::RenderGraph::ResourceKind::Image);

    VulkanEngine::RenderGraph::TransientImageInfo info{};
    info.format = desc.format;
    info.width = desc.width;
    info.height = desc.height;
    info.mip_levels = 1;
    info.array_layers = 1;
    info.sample_count = vk::SampleCountFlagBits::e1;
    info.usage = desc.usage;
    info.tiling = vk::ImageTiling::eOptimal;

    graph_builder_.SetTransientImageInfo(handle, info);

    if (desc.initial_layout != vk::ImageLayout::eUndefined) {
        auto initial_state = VulkanEngine::RenderGraph::ResourceState::ImageState(
            VulkanEngine::RenderGraph::PipelineStageIntent::TopOfPipe,
            VulkanEngine::RenderGraph::AccessIntent::None,
            VulkanEngine::RenderGraph::QueueType::Graphics,
            desc.initial_layout == vk::ImageLayout::eColorAttachmentOptimal ? VulkanEngine::RenderGraph::ImageLayoutIntent::ColorAttachment :
            desc.initial_layout == vk::ImageLayout::eDepthAttachmentOptimal ? VulkanEngine::RenderGraph::ImageLayoutIntent::DepthAttachment :
            desc.initial_layout == vk::ImageLayout::eShaderReadOnlyOptimal ? VulkanEngine::RenderGraph::ImageLayoutIntent::ShaderReadOnly :
            VulkanEngine::RenderGraph::ImageLayoutIntent::Undefined);
        graph_builder_.SetInitialState(handle, initial_state);
    }

    if (desc.final_layout != vk::ImageLayout::eUndefined) {
        auto final_state = VulkanEngine::RenderGraph::ResourceState::ImageState(
            VulkanEngine::RenderGraph::PipelineStageIntent::BottomOfPipe,
            VulkanEngine::RenderGraph::AccessIntent::None,
            VulkanEngine::RenderGraph::QueueType::Graphics,
            desc.final_layout == vk::ImageLayout::eColorAttachmentOptimal ? VulkanEngine::RenderGraph::ImageLayoutIntent::ColorAttachment :
            desc.final_layout == vk::ImageLayout::eDepthAttachmentOptimal ? VulkanEngine::RenderGraph::ImageLayoutIntent::DepthAttachment :
            desc.final_layout == vk::ImageLayout::ePresentSrcKHR ? VulkanEngine::RenderGraph::ImageLayoutIntent::Present :
            VulkanEngine::RenderGraph::ImageLayoutIntent::Undefined);
        graph_builder_.SetFinalState(handle, final_state);
    }

    const uint32_t res_index = handle.index;
    transient_image_descs_[res_index] = desc;

    return handle;
}

VulkanEngine::RenderGraph::PassHandle RenderPipeline::AddPass(const RenderPipelinePassDesc& desc) {
    VulkanEngine::RenderGraph::PassExecutionCallback callback{};
    callback.callback = desc.execute;

    auto handle = graph_builder_.AddPass(desc.name, desc.queue, true, callback);

    for (const auto& read : desc.reads) {
        graph_builder_.AddRead(handle, read);
    }
    for (const auto& write : desc.writes) {
        graph_builder_.AddWrite(handle, write);
    }

    if (desc.attachments) {
        graph_builder_.SetPassAttachments(handle, *desc.attachments);
    }

    return handle;
}

bool RenderPipeline::SetInitialState(VulkanEngine::RenderGraph::ResourceHandle resource, VulkanEngine::RenderGraph::ResourceState state) {
    return graph_builder_.SetInitialState(resource, state);
}

bool RenderPipeline::SetFinalState(VulkanEngine::RenderGraph::ResourceHandle resource, VulkanEngine::RenderGraph::ResourceState state) {
    return graph_builder_.SetFinalState(resource, state);
}

void RenderPipeline::Compile() {
    compiled_graph_ = graph_builder_.Compile();
    compiled_ = compiled_graph_.success;

    if (compiled_) {
        AllocateTransients();
    }
}

void RenderPipeline::SetImportedResourceState(uint32_t resource_index, VulkanEngine::RenderGraph::ResourceState state) {
    pending_imported_states_[resource_index] = state;
}

void RenderPipeline::Execute(const void* user_data, vk::CommandBuffer command_buffer, uint32_t image_index) {
    if (!compiled_ || !initialized_) {
        return;
    }

    auto resolved_graph = compiled_graph_;

    for (const auto& [index, state] : pending_imported_states_) {
        resolved_graph.SetImportedResourceState(index, state);
    }
    pending_imported_states_.clear();

    ResolveResources(resolved_graph, image_index);

    resolved_graph.Execute(user_data, command_buffer);
}

void RenderPipeline::AllocateTransients() {
    if (!bootstrap_) {
        return;
    }

    auto& backend = bootstrap_->GetBackend();
    const auto& device = backend.GetDevice();
    const auto& physical_device = backend.GetPhysicalDevice();

    for (const auto& [index, desc] : transient_image_descs_) {
        if (desc.width == 0 || desc.height == 0 || desc.format == vk::Format::eUndefined) {
            continue;
        }

        vk::ImageCreateInfo image_info{};
        image_info.imageType = vk::ImageType::e2D;
        image_info.format = desc.format;
        image_info.extent = vk::Extent3D{desc.width, desc.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = vk::SampleCountFlagBits::e1;
        image_info.tiling = vk::ImageTiling::eOptimal;
        image_info.usage = desc.usage;
        image_info.initialLayout = vk::ImageLayout::eUndefined;

        transient_images_.emplace_back(device, image_info);

        const auto mem_requirements = transient_images_.back().getMemoryRequirements();
        vk::MemoryAllocateInfo alloc_info{};
        alloc_info.allocationSize = mem_requirements.size;
        alloc_info.memoryTypeIndex = FindMemoryType(physical_device, mem_requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

        transient_memories_.emplace_back(device, alloc_info);
        transient_images_.back().bindMemory(transient_memories_.back(), 0);

        vk::ImageViewCreateInfo view_info{};
        view_info.image = *transient_images_.back();
        view_info.viewType = vk::ImageViewType::e2D;
        view_info.format = desc.format;
        view_info.subresourceRange = {FormatToAspectFlags(desc.format), 0, 1, 0, 1};

        transient_image_views_.emplace_back(device, view_info);
    }
}

void RenderPipeline::DeallocateTransients() {
    transient_image_views_.clear();
    transient_memories_.clear();
    transient_images_.clear();
    transient_image_descs_.clear();
}

void RenderPipeline::ResolveResources(VulkanEngine::RenderGraph::CompiledRenderGraph& graph, uint32_t image_index) {
    if (!bootstrap_) {
        return;
    }

    auto& backend = bootstrap_->GetBackend();

    for (size_t i = 0; i < graph.resource_lifetimes.size(); ++i) {
        const auto& resource = graph.resource_lifetimes[i];

        if (resource.imported) {
            if (resource.name == "swapchain-backbuffer") {
                graph.SetResourceImage(static_cast<uint32_t>(i), backend.GetSwapchainImages()[image_index]);
                graph.SetResourceFormat(static_cast<uint32_t>(i), static_cast<vk::Format>(backend.GetSurfaceFormat().format));
            } else if (resource.name == "depth-buffer") {
                graph.SetResourceImage(static_cast<uint32_t>(i), *backend.GetDepthImage());
                graph.SetResourceFormat(static_cast<uint32_t>(i), backend.GetDepthFormat());
            }
        } else {
            if (i < transient_images_.size()) {
                graph.SetResourceImage(static_cast<uint32_t>(i), *transient_images_[i]);
            }
            auto it = transient_image_descs_.find(static_cast<uint32_t>(i));
            if (it != transient_image_descs_.end()) {
                graph.SetResourceFormat(static_cast<uint32_t>(i), it->second.format);
            }
        }
    }

    for (auto& pass : graph.passes) {
        if (pass.attachment_setup && pass.attachment_setup->auto_begin_rendering) {
            auto resolve_attachment_view = [&](VulkanEngine::RenderGraph::AttachmentInfo& attach) {
                const auto& resource = graph.resource_lifetimes[attach.resource.index];
                if (resource.imported) {
                    if (resource.name == "swapchain-backbuffer") {
                        attach.image_view = *backend.GetSwapchainImageViews()[image_index];
                    } else if (resource.name == "depth-buffer") {
                        attach.image_view = *backend.GetDepthImageView();
                    }
                } else {
                    if (attach.resource.index < transient_image_views_.size()) {
                        attach.image_view = *transient_image_views_[attach.resource.index];
                    }
                }
            };

            auto& setup = *pass.attachment_setup;
            for (auto& attach : setup.color_attachments) {
                resolve_attachment_view(attach);
            }
            if (setup.depth_attachment.has_value()) {
                resolve_attachment_view(*setup.depth_attachment); //NOLINT(bugprone-unchecked-optional-access)
            }

            uint32_t max_width = 0, max_height = 0;
            for (const auto& attach : setup.color_attachments) {
                if (attach.resource.index < transient_image_descs_.size()) {
                    const auto& desc = transient_image_descs_[attach.resource.index];
                    max_width = std::max(max_width, desc.width);
                    max_height = std::max(max_height, desc.height);
                }
            }
            if (max_width == 0) {
                (void)backend.GetSwapchainExtent(max_width, max_height);
            }
            setup.render_area = vk::Rect2D{{0, 0}, {max_width, max_height}};
        }
    }
}

} // namespace VulkanEngine::RenderPipeline
