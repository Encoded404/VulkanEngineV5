module;

module VulkanEngine.RenderPipeline;

import std;

import vulkan_hpp;

import VulkanEngine.RenderGraph;
import VulkanBackend.Vulkan.RenderGraphExecutor;
import VulkanBackend.Vulkan.VulkanBootstrap;
import VulkanBackend.Vulkan.VulkanDebugUtils;

namespace VulkanEngine::RenderPipeline {

namespace {

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

void RenderPipeline::Initialize(VulkanBackend::Vulkan::VulkanBootstrap& bootstrap) {
    bootstrap_ = &bootstrap;
    initialized_ = true;

    auto& backend = bootstrap.GetBackend();
    const auto surface_format = static_cast<vk::Format>(backend.GetSurfaceFormat().format);
    const auto depth_format = backend.GetDepthFormat();

    RegisterResourceResolver("swapchain-backbuffer",
        [&backend](std::uint32_t img_idx) { return backend.GetSwapchainImages()[img_idx]; },
        [&backend](std::uint32_t img_idx) { return *backend.GetSwapchainImageViews()[img_idx]; },
        surface_format);
    RegisterResourceResolver("depth-buffer",
        [&backend](std::uint32_t img_idx) { return *backend.GetDepthImage(img_idx); },
        [&backend](std::uint32_t img_idx) { return *backend.GetDepthImageView(img_idx); },
        depth_format);
}

void RenderPipeline::Shutdown() {
    transient_image_descs_.clear();
    transient_buffer_descs_.clear();
    transient_allocator_.Shutdown();
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

VulkanEngine::RenderGraph::ResourceHandle RenderPipeline::ImportImage(const std::string& name) {
    return graph_builder_.ImportResource(name, VulkanEngine::RenderGraph::ResourceKind::Image);
}

VulkanEngine::RenderGraph::ResourceHandle RenderPipeline::ImportBuffer(const std::string& name) {
    return graph_builder_.ImportResource(name, VulkanEngine::RenderGraph::ResourceKind::Buffer);
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
    info.aliasable = desc.aliasable;

    graph_builder_.SetTransientImageInfo(handle, info);

    // ContentsUndefined contract: an aliasable image must start Undefined, so a
    // requested non-Undefined initial layout is dropped rather than aliased.
    if (!desc.aliasable && desc.initial_layout != vk::ImageLayout::eUndefined) {
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

    const std::uint32_t res_index = handle.index;
    transient_image_descs_[res_index] = desc;

    return handle;
}

VulkanEngine::RenderGraph::ResourceHandle RenderPipeline::CreateTransientBuffer(const TransientBufferDesc& desc) {
    auto handle = graph_builder_.CreateTransientResource(desc.name, VulkanEngine::RenderGraph::ResourceKind::Buffer);

    VulkanEngine::RenderGraph::TransientBufferInfo info{};
    info.name = desc.name;
    info.size = desc.size;
    info.usage = desc.usage;
    info.memory_properties = desc.memory_properties;
    info.aliasable = desc.aliasable;

    graph_builder_.SetTransientBufferInfo(handle, info);

    const std::uint32_t res_index = handle.index;
    transient_buffer_descs_[res_index] = desc;

    return handle;
}

void RenderPipeline::RegisterResourceResolver(const std::string& name,
                                               ImageResolver resolve_image,
                                               ImageViewResolver resolve_image_view,
                                               vk::Format format) {
    resource_resolvers_[name] = ExternalResourceResolver{
        .resolve_image = std::move(resolve_image),
        .resolve_image_view = std::move(resolve_image_view),
        .format = format
    };
}

void RenderPipeline::RegisterBufferResolver(const std::string& name, BufferResolver resolve_buffer) {
    buffer_resolvers_[name] = std::move(resolve_buffer);
}

VulkanEngine::RenderGraph::PassHandle RenderPipeline::AddPass(const RenderPipelinePassDesc& desc) {
    VulkanEngine::RenderGraph::PassExecutionCallback callback{};
    callback.callback = desc.execute;

    auto handle = graph_builder_.AddPass(desc.name, desc.queue, true, callback);

    for (const auto& read : desc.reads) {
        graph_builder_.AddRead(handle, read.resource, read.stage, read.access);
    }
    for (const auto& write : desc.writes) {
        graph_builder_.AddWrite(handle, write);
    }

    if (desc.attachments) {
        graph_builder_.SetPassAttachments(handle, *desc.attachments);
    }

    return handle;
}

VulkanEngine::RenderGraph::PassHandle RenderPipeline::AddCustomPass(
    std::unique_ptr<VulkanEngine::PipelinePass::IPipelinePass> pass,
    VulkanEngine::PipelinePass::PassSetupContext& ctx) {
    // Store the pass for lifetime
    custom_passes_.push_back(std::move(pass));
    auto* pass_ptr = custom_passes_.back().get();

    // Build a RenderPipelinePassDesc from the context's declarations
    RenderPipelinePassDesc desc;
    desc.name = "CustomPass-" + std::to_string(custom_passes_.size());

    // ── Wire up declared resource reads ──
    const auto& read_resources = ctx.GetReadResources();
    const auto& read_stages = ctx.GetReadStages();
    const auto& read_accesses = ctx.GetReadAccesses();
    for (std::size_t i = 0; i < read_resources.size(); ++i) {
        desc.reads.push_back({
            .resource = read_resources[i],
            .stage = i < read_stages.size() ? read_stages[i]
                     : VulkanEngine::RenderGraph::PipelineStageIntent::FragmentShader,
            .access = i < read_accesses.size() ? read_accesses[i]
                      : VulkanEngine::RenderGraph::AccessIntent::Read,
        });
    }

    // ── Wire up declared writes ──
    desc.writes = ctx.GetWriteResources();

    // ── Set attachment info ──
    if (ctx.GetAttachmentSetup()) {
        desc.attachments = *ctx.GetAttachmentSetup();
    }

    // ── Create execute callback ──
    desc.execute = [pass_ptr](const void* /*user_data*/, vk::CommandBuffer cmd) {
        // TODO: Populate FrameContext with per-frame data from user_data.
        // The user_data is passed through from RenderPipeline::Execute()
        // and will contain frame-specific resources (descriptor sets, etc.).
        // Required fields to populate:
        //   - view_proj: camera view-projection matrix (consumed by ExpandPass)
        //   - render_extent: current swapchain extent
        //   - frame_index, swapchain_image_index: from Renderer::RenderFrame()
        const VulkanEngine::PipelinePass::FrameContext frame_ctx{};
        pass_ptr->Execute(frame_ctx, cmd);
    };

    // Register with the graph builder via the existing AddPass path
    auto handle = AddPass(desc);

    // ── Resolve RunBefore/RunAfter ordering with builtin passes ──
    for (auto bp : ctx.GetBeforeBuiltinPasses()) {
        auto idx = static_cast<std::size_t>(bp);
        if (idx < builtin_handles_.size() && builtin_handles_[idx].IsValid()) {
            AddDependency(handle, builtin_handles_[idx]);
        }
    }
    for (auto bp : ctx.GetAfterBuiltinPasses()) {
        auto idx = static_cast<std::size_t>(bp);
        if (idx < builtin_handles_.size() && builtin_handles_[idx].IsValid()) {
            AddDependency(builtin_handles_[idx], handle);
        }
    }

    return handle;
}

void RenderPipeline::SetBuiltinHandles(const std::array<VulkanEngine::RenderGraph::PassHandle, 6>& handles) {
    builtin_handles_ = handles;
}

const std::array<VulkanEngine::RenderGraph::PassHandle, 6>& RenderPipeline::GetBuiltinHandles() const {
    return builtin_handles_;
}

bool RenderPipeline::AddDependency(VulkanEngine::RenderGraph::PassHandle before,
                                   VulkanEngine::RenderGraph::PassHandle after) {
    return graph_builder_.AddDependency(before, after).has_value();
}

bool RenderPipeline::SetInitialState(VulkanEngine::RenderGraph::ResourceHandle resource, VulkanEngine::RenderGraph::ResourceState state) {
    return graph_builder_.SetInitialState(resource, state).has_value();
}

bool RenderPipeline::SetFinalState(VulkanEngine::RenderGraph::ResourceHandle resource, VulkanEngine::RenderGraph::ResourceState state) {
    return graph_builder_.SetFinalState(resource, state).has_value();
}

void RenderPipeline::Compile() {
    compiled_graph_ = graph_builder_.Compile();
    compiled_ = compiled_graph_.success;

    if (compiled_) {
        SyncTransients();

        for (std::size_t i = 0; i < compiled_graph_.resource_lifetimes.size(); ++i) {
            const auto& resource = compiled_graph_.resource_lifetimes[i];
            if (resource.name == "swapchain-backbuffer") {
                backbuffer_resource_index_ = static_cast<std::uint32_t>(i);
            } else if (resource.name == "depth-buffer") {
                depth_buffer_resource_index_ = static_cast<std::uint32_t>(i);
            }
        }
    }
}

void RenderPipeline::Execute(const void* user_data, vk::CommandBuffer command_buffer,
                             std::uint32_t image_index, std::uint32_t fif_slot) {
    if (!compiled_ || !initialized_) {
        return;
    }

    auto resolved_graph = compiled_graph_;

    if (!bootstrap_) return;

    transient_allocator_.CollectGarbage(fif_slot);

    resolved_graph.SetImportedResourceState(backbuffer_resource_index_,
        VulkanEngine::RenderGraph::ResourceState::ImageState(
            VulkanEngine::RenderGraph::PipelineStageIntent::TopOfPipe,
            VulkanEngine::RenderGraph::AccessIntent::None,
            VulkanEngine::RenderGraph::QueueType::Graphics,
            VulkanEngine::RenderGraph::ImageLayoutIntent::Undefined));

    resolved_graph.SetImportedResourceState(depth_buffer_resource_index_,
        VulkanEngine::RenderGraph::ResourceState::ImageState(
            VulkanEngine::RenderGraph::PipelineStageIntent::TopOfPipe,
            VulkanEngine::RenderGraph::AccessIntent::None,
            VulkanEngine::RenderGraph::QueueType::Graphics,
            VulkanEngine::RenderGraph::ImageLayoutIntent::Undefined));

    ResolveResources(resolved_graph, image_index, fif_slot);

    VulkanEngine::RenderGraph::ResolvedResourceHandles resolved{};
    resolved.images = resolved_graph.resource_images;
    resolved.buffers = resolved_graph.resource_buffers;
    resolved.buffer_offsets = resolved_graph.resource_buffer_offsets;
    resolved.buffer_sizes = resolved_graph.resource_buffer_sizes;
    resolved.formats = resolved_graph.resource_formats;

    // Alias reuse decided by the transient planner becomes explicit ordering
    // dependencies in the barrier plan.
    VulkanEngine::RenderGraph::AliasIntervals alias_intervals{};
    for (const auto& alias : transient_allocator_.GetPlan().aliases) {
        alias_intervals.dependencies.push_back(VulkanEngine::RenderGraph::PlannedAliasDependency{
            .aliased_resource = alias.aliased_resource,
            .after_resource = alias.after_resource,
            .pass_index = alias.pass_index,
        });
    }

    const auto plan = VulkanEngine::RenderGraph::PlanBarriers(
        resolved_graph, resolved, alias_intervals);

    VulkanBackend::Vulkan::ExecuteRenderGraph(plan, resolved_graph, user_data, command_buffer);
}

void RenderPipeline::SyncTransients() {
    if (!bootstrap_) {
        return;
    }

    auto& backend = bootstrap_->GetBackend();
    if (!transient_allocator_.IsInitialized()) {
        transient_allocator_.Initialize(backend, "render-pipeline-transients");
    }

    const std::uint32_t frames_in_flight = std::max<std::uint32_t>(backend.GetFramesInFlight(), 1);

    std::vector<VulkanEngine::GpuResources::TransientAllocator::Desc> descs;
    descs.reserve(transient_image_descs_.size() + transient_buffer_descs_.size());

    for (const auto& lifetime : compiled_graph_.resource_lifetimes) {
        if (!lifetime.transient) {
            continue;
        }
        const std::uint32_t index = lifetime.handle.index;
        const bool is_buffer =
            index < compiled_graph_.resource_info.size() &&
            compiled_graph_.resource_info[index].kind == VulkanEngine::RenderGraph::ResourceKind::Buffer;

        VulkanEngine::GpuResources::TransientAllocator::Desc desc{};
        desc.requirements.name = lifetime.name;
        desc.requirements.first_pass = lifetime.first_pass;
        desc.requirements.last_pass = lifetime.last_pass;

        if (is_buffer) {
            const auto it = transient_buffer_descs_.find(index);
            if (it == transient_buffer_descs_.end() || it->second.size == 0) {
                continue;
            }
            desc.is_image = false;
            desc.requirements.kind = VulkanEngine::GpuResources::TransientKind::Buffer;
            desc.requirements.heap_key = 1;
            desc.requirements.size = it->second.size;
            desc.requirements.alignment = 256;
            desc.requirements.aliasable = it->second.aliasable;
            desc.buffer.usage = it->second.usage;
            desc.buffer.memory = it->second.memory_properties;
        } else {
            const auto it = transient_image_descs_.find(index);
            if (it == transient_image_descs_.end() || it->second.width == 0 || it->second.height == 0 ||
                it->second.format == vk::Format::eUndefined) {
                continue;
            }
            desc.is_image = true;
            desc.requirements.kind = VulkanEngine::GpuResources::TransientKind::Image;
            desc.requirements.heap_key = 0;
            // Sized from the real image memory requirements by the allocator.
            desc.requirements.size = 0;
            desc.requirements.alignment = 1;
            desc.requirements.aliasable = it->second.aliasable;
            desc.image.format = it->second.format;
            desc.image.width = it->second.width;
            desc.image.height = it->second.height;
            desc.image.usage = it->second.usage;
            desc.image.samples = vk::SampleCountFlagBits::e1;
            desc.image.aspect = FormatToAspectFlags(it->second.format);
        }

        descs.push_back(std::move(desc));
    }

    // A recompile happens at a frame boundary; frame 0 retires against the next
    // submitted frame, so nothing in flight is freed early.
    transient_allocator_.Sync(descs, frames_in_flight, 0);
}

void RenderPipeline::ResolveResources(VulkanEngine::RenderGraph::CompiledRenderGraph& graph,
                                      std::uint32_t image_index, std::uint32_t fif_slot) {
    if (!bootstrap_) {
        return;
    }

    auto& backend = bootstrap_->GetBackend();

    for (std::size_t i = 0; i < graph.resource_lifetimes.size(); ++i) {
        const auto& resource = graph.resource_lifetimes[i];
        const bool is_buffer = i < graph.resource_info.size() &&
                               graph.resource_info[i].kind == VulkanEngine::RenderGraph::ResourceKind::Buffer;

        if (resource.imported) {
            if (is_buffer) {
                auto buffer_it = buffer_resolvers_.find(resource.name);
                if (buffer_it != buffer_resolvers_.end()) {
                    graph.SetResourceBuffer(static_cast<std::uint32_t>(i), buffer_it->second(image_index));
                }
            } else {
                auto it = resource_resolvers_.find(resource.name);
                if (it != resource_resolvers_.end()) {
                    graph.SetResourceImage(static_cast<std::uint32_t>(i), it->second.resolve_image(image_index));
                    graph.SetResourceFormat(static_cast<std::uint32_t>(i), it->second.format);
                }
            }
        } else if (is_buffer) {
            vk::Buffer buffer{};
            vk::DeviceSize offset = 0;
            vk::DeviceSize size = vk::WholeSize;
            if (transient_allocator_.GetBuffer(static_cast<std::uint32_t>(i), fif_slot, buffer, offset, size)) {
                graph.SetResourceBuffer(static_cast<std::uint32_t>(i), buffer, offset, size);
            }
        } else {
            const vk::Image image = transient_allocator_.GetImage(static_cast<std::uint32_t>(i), fif_slot);
            if (image) {
                graph.SetResourceImage(static_cast<std::uint32_t>(i), image);
            }
            auto it = transient_image_descs_.find(static_cast<std::uint32_t>(i));
            if (it != transient_image_descs_.end()) {
                graph.SetResourceFormat(static_cast<std::uint32_t>(i), it->second.format);
            }
        }
    }

    for (auto& pass : graph.passes) {
        if (pass.attachment_setup && pass.attachment_setup->auto_begin_rendering) {
            auto resolve_attachment_view = [&](VulkanEngine::RenderGraph::AttachmentInfo& attach) {
                const auto& resource = graph.resource_lifetimes[attach.resource.index];
                if (resource.imported) {
                    auto it = resource_resolvers_.find(resource.name);
                    if (it != resource_resolvers_.end()) {
                        attach.image_view = it->second.resolve_image_view(image_index);
                    }
                } else {
                    const vk::ImageView view = transient_allocator_.GetImageView(attach.resource.index, fif_slot);
                    if (view) {
                        attach.image_view = view;
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

            std::uint32_t max_width = 0, max_height = 0;
            for (const auto& attach : setup.color_attachments) {
                auto desc_it = transient_image_descs_.find(attach.resource.index);
                if (desc_it != transient_image_descs_.end()) {
                    const auto& desc = desc_it->second;
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
