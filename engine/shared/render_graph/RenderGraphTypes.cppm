module;
export module VulkanShared.RenderGraphTypes;
import std;
import vulkan_hpp;

export namespace VulkanEngine::RenderGraph {

enum class ResourceKind : std::uint8_t {
    Image,
    Buffer
};

struct ResourceHandle {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool IsValid() const noexcept {
        return index != std::numeric_limits<std::uint32_t>::max();
    }

    friend bool operator==(const ResourceHandle&, const ResourceHandle&) = default;
};

struct PassHandle {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool IsValid() const noexcept {
        return index != std::numeric_limits<std::uint32_t>::max();
    }

    friend bool operator==(const PassHandle&, const PassHandle&) = default;
};

// ── Execution model enums ──

enum class PipelineStageIntent : std::uint8_t {
    None,
    Transfer,
    ColorAttachment,
    DepthAttachment,
    VertexShader,
    // Vertex/index fetch: VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, distinct from the
    // vertex-shader stage. Also covers the vertex shader reads on draw passes
    // so one intent synchronizes a compute-written buffer consumed as both an
    // index buffer and a shader resource.
    IndexInput,
    IndirectDraw,
    FragmentShader,
    ComputeShader,
    Present,
    TopOfPipe,
    BottomOfPipe
};

enum class AccessIntent : std::uint8_t {
    None,
    Read,
    Write,
    ReadWrite
};

enum class ImageLayoutIntent : std::uint8_t {
    Undefined,
    General,
    ColorAttachment,
    DepthAttachment,
    ShaderReadOnly,
    TransferSource,
    TransferDestination,
    Present,
    DepthReadOnly
};

enum class QueueType : std::uint8_t {
    Graphics,
    Compute,
    Transfer
};

// ── Resource state tracking ──

struct ResourceState {
    PipelineStageIntent stage = PipelineStageIntent::None;
    AccessIntent access = AccessIntent::None;
    QueueType queue = QueueType::Graphics;
    ImageLayoutIntent layout = ImageLayoutIntent::Undefined;
    bool has_image_layout = false;

    [[nodiscard]] static ResourceState BufferState(PipelineStageIntent stage_intent,
                                                    AccessIntent access_intent,
                                                    QueueType queue_intent = QueueType::Graphics) {
        return ResourceState{
            .stage = stage_intent,
            .access = access_intent,
            .queue = queue_intent,
            .layout = ImageLayoutIntent::Undefined,
            .has_image_layout = false,
        };
    }

    [[nodiscard]] static ResourceState ImageState(PipelineStageIntent stage_intent,
                                                   AccessIntent access_intent,
                                                   QueueType queue_intent,
                                                   ImageLayoutIntent layout_intent) {
        return ResourceState{
            .stage = stage_intent,
            .access = access_intent,
            .queue = queue_intent,
            .layout = layout_intent,
            .has_image_layout = true,
        };
    }
};

// ── Compiler diagnostics ──

enum class DiagnosticSeverity : std::uint8_t {
    Info,
    Warning,
    Error
};

enum class DiagnosticCode : std::uint8_t {
    None,
    EmptyGraph,
    InvalidExplicitDependency,
    InvalidReadHandle,
    InvalidWriteHandle,
    InvalidInitialState,
    InvalidFinalState,
    CycleDetected,
    CompileSuccess
};

struct CompileDiagnostic {
    DiagnosticCode code = DiagnosticCode::None;
    DiagnosticSeverity severity = DiagnosticSeverity::Info;
    std::string message{};
};

// ── Transient resource descriptions ──

struct TransientImageInfo {
    vk::Format format = vk::Format::eUndefined;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mip_levels = 1;
    std::uint32_t array_layers = 1;
    vk::SampleCountFlagBits sample_count = vk::SampleCountFlagBits::e1;
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment;
    vk::ImageTiling tiling = vk::ImageTiling::eOptimal;
    // Reserved for Phase 2 interval aliasing. Aliasable images must be created
    // with VK_IMAGE_CREATE_ALIAS_BIT and must carry an Undefined initial
    // layout, which Phase 2 enforces.
    bool aliasable = false;
};

struct TransientBufferInfo {
    vk::DeviceSize size = 0;
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eStorageBuffer;
    vk::MemoryPropertyFlags memory_properties = vk::MemoryPropertyFlagBits::eDeviceLocal;
    bool aliasable = false;
};

// ── Resource metadata ──

struct ResourceInfo {
    std::string name{};
    ResourceKind kind = ResourceKind::Image;
    bool imported = false;
    std::optional<TransientImageInfo> image_info{};
    std::optional<TransientBufferInfo> buffer_info{};
};

// ── Pass execution ──

struct PassExecutionCallback {
    std::function<void(const void* user_data, vk::CommandBuffer command_buffer)> callback{};
};

// ── Attachment types ──

struct AttachmentInfo {
    ResourceHandle resource{};
    vk::ImageView image_view = {};
    vk::AttachmentLoadOp load_op = vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp store_op = vk::AttachmentStoreOp::eStore;
    vk::ClearColorValue clear_color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
    vk::ClearDepthStencilValue clear_depth = vk::ClearDepthStencilValue(1.0f, 0);
};

struct PassAttachmentSetup {
    std::vector<AttachmentInfo> color_attachments{};
    std::optional<AttachmentInfo> depth_attachment{};
    vk::Rect2D render_area = {};
    bool auto_begin_rendering = false;
};

// ── Planned transitions ──
//
// A ResourceTransition is resolved entirely by the compiler: `from_state` is
// the state the resource was left in by the previous use (or the Undefined
// state when `from_known` is false), and the stage/access flag pairs are the
// OR of every use this pass makes of the resource. Read+write on the same
// resource in one pass therefore collapses into a single transition, which is
// what makes the emitted barrier correct for a combined read/write hazard.
struct ResourceTransition {
    std::uint32_t resource_index = std::numeric_limits<std::uint32_t>::max();
    ResourceState from_state{};
    ResourceState target_state{};
    bool from_known = false;
    vk::PipelineStageFlags2 src_stage{};
    vk::AccessFlags2 src_access{};
    vk::PipelineStageFlags2 dst_stage{};
    vk::AccessFlags2 dst_access{};
};

// ── Compiled pass description ──

struct CompiledPass {
    PassHandle handle{};
    std::string name{};
    QueueType queue = QueueType::Graphics;
    std::vector<ResourceTransition> pre_pass_transitions{};
    std::vector<ResourceTransition> post_pass_transitions{};
    PassExecutionCallback execute{};
    std::optional<PassAttachmentSetup> attachment_setup{};
};

// ── Resource lifetime tracking (compiler output) ──

struct ResourceLifetime {
    ResourceHandle handle{};
    std::string name{};
    bool imported = false;
    bool transient = false;
    std::int32_t first_pass = -1;
    std::int32_t last_pass = -1;
};

// ── Compiled render graph (execution plan) ──

struct CompiledRenderGraph {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    bool success = false;
    std::vector<CompileDiagnostic> diagnostics{};
    std::vector<CompiledPass> passes{};
    std::vector<ResourceLifetime> resource_lifetimes{};
    std::vector<ResourceInfo> resource_info{};
    mutable std::vector<ResourceState> initial_states{};
    mutable std::vector<bool> has_initial_state{};
    std::vector<vk::Image> resource_images{};
    std::vector<vk::Buffer> resource_buffers{};
    std::vector<vk::Format> resource_formats{};
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    void SetImportedResourceState(std::uint32_t resource_index, ResourceState state) const {
        if (resource_index < initial_states.size()) {
            initial_states[resource_index] = state;
            has_initial_state[resource_index] = true;
        }
    }

    void SetResourceImage(std::uint32_t resource_index, vk::Image image) {
        if (resource_index < resource_images.size()) {
            resource_images[resource_index] = image;
        }
    }

    void SetResourceBuffer(std::uint32_t resource_index, vk::Buffer buffer) {
        if (resource_index < resource_buffers.size()) {
            resource_buffers[resource_index] = buffer;
        }
    }

    void SetResourceFormat(std::uint32_t resource_index, vk::Format format) {
        if (resource_index < resource_formats.size()) {
            resource_formats[resource_index] = format;
        }
    }
};

// ── Resolved handles + barrier plan (pure planner output) ──

// Vulkan handles resolved at the execute boundary, indexed by resource index.
// Kept separate from CompiledRenderGraph so the planner stays a pure function
// over graph data + handles and never touches the device.
struct ResolvedResourceHandles {
    std::vector<vk::Image> images{};
    std::vector<vk::Buffer> buffers{};
    std::vector<vk::Format> formats{};
};

// Reserved for Phase 2. Empty in Phase 1: no alias reuse is planned, but the
// planner signature and BarrierPlan carry the seam so Phase 2 can fill it
// without an API break.
struct PlannedAliasDependency {
    std::uint32_t aliased_resource = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t after_resource = std::numeric_limits<std::uint32_t>::max();
    std::int32_t pass_index = -1;
};

struct AliasIntervals {
    std::vector<PlannedAliasDependency> dependencies{};
};

struct PlannedImageBarrier {
    std::uint32_t resource_index = std::numeric_limits<std::uint32_t>::max();
    vk::Image image = {};
    vk::PipelineStageFlags2 src_stage{};
    vk::PipelineStageFlags2 dst_stage{};
    vk::AccessFlags2 src_access{};
    vk::AccessFlags2 dst_access{};
    vk::ImageLayout old_layout = vk::ImageLayout::eUndefined;
    vk::ImageLayout new_layout = vk::ImageLayout::eUndefined;
    vk::ImageSubresourceRange range{};
};

struct PlannedBufferBarrier {
    std::uint32_t resource_index = std::numeric_limits<std::uint32_t>::max();
    // Null when the engine could not resolve a concrete buffer for this
    // resource. The executor then falls back to a conservative global memory
    // barrier with the same scopes; correctness is preserved, only the
    // precision is lost until a resolver is registered.
    vk::Buffer buffer = {};
    vk::PipelineStageFlags2 src_stage{};
    vk::PipelineStageFlags2 dst_stage{};
    vk::AccessFlags2 src_access{};
    vk::AccessFlags2 dst_access{};
    vk::DeviceSize offset = 0;
    vk::DeviceSize size = vk::WholeSize;
};

struct PlannedPassBarriers {
    std::uint32_t pass_index = std::numeric_limits<std::uint32_t>::max();
    std::string pass_name{};
    std::vector<PlannedImageBarrier> pre_image{};
    std::vector<PlannedBufferBarrier> pre_buffer{};
    std::vector<PlannedImageBarrier> post_image{};
    std::vector<PlannedBufferBarrier> post_buffer{};
};

struct BarrierPlan {
    bool valid = false;
    std::vector<PlannedPassBarriers> passes{};

    [[nodiscard]] bool HasBarriers() const {
        for (const auto& pass : passes) {
            if (!pass.pre_image.empty() || !pass.pre_buffer.empty() ||
                !pass.post_image.empty() || !pass.post_buffer.empty()) {
                return true;
            }
        }
        return false;
    }
};

// ── Helper functions for state mapping ──

inline bool StatesEqual(const ResourceState& a, const ResourceState& b) {
    if (a.stage != b.stage) return false;
    if (a.access != b.access) return false;
    if (a.queue != b.queue) return false;
    if (a.has_image_layout != b.has_image_layout) return false;
    if (a.has_image_layout && a.layout != b.layout) return false;
    return true;
}

// sync2 (core in Vulkan 1.3). TopOfPipe/BottomOfPipe/Present collapse to eNone:
// the deprecated top/bottom stages have no sync2 equivalent, and an Undefined
// source needs no source scope. Callers must OR these into 64-bit masks.
inline vk::PipelineStageFlags2 IntentToPipelineStage(PipelineStageIntent intent, AccessIntent access) {
    (void)access;
    switch (intent) {
        case PipelineStageIntent::None:
        case PipelineStageIntent::Present:
        case PipelineStageIntent::TopOfPipe:
        case PipelineStageIntent::BottomOfPipe:
            return vk::PipelineStageFlagBits2::eNone;
        case PipelineStageIntent::Transfer:
            return vk::PipelineStageFlagBits2::eTransfer;
        case PipelineStageIntent::ColorAttachment:
            return vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        case PipelineStageIntent::DepthAttachment:
            return vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                   vk::PipelineStageFlagBits2::eLateFragmentTests;
        case PipelineStageIntent::VertexShader:
            return vk::PipelineStageFlagBits2::eVertexShader;
        case PipelineStageIntent::IndexInput:
            return vk::PipelineStageFlagBits2::eVertexInput |
                   vk::PipelineStageFlagBits2::eVertexShader;
        case PipelineStageIntent::FragmentShader:
            return vk::PipelineStageFlagBits2::eFragmentShader;
        case PipelineStageIntent::ComputeShader:
            return vk::PipelineStageFlagBits2::eComputeShader;
        case PipelineStageIntent::IndirectDraw:
            return vk::PipelineStageFlagBits2::eDrawIndirect;
    }
    return vk::PipelineStageFlagBits2::eNone;
}

inline vk::AccessFlags2 IntentToAccessFlags(PipelineStageIntent stage, AccessIntent access) {
    if (access == AccessIntent::None) return {};

    auto stage_access = [](PipelineStageIntent s, bool is_write) -> vk::AccessFlags2 {
        switch (s) {
            case PipelineStageIntent::Transfer:
                return is_write ? vk::AccessFlagBits2::eTransferWrite
                                : vk::AccessFlagBits2::eTransferRead;
            case PipelineStageIntent::ColorAttachment:
                return vk::AccessFlagBits2::eColorAttachmentWrite;
            case PipelineStageIntent::DepthAttachment:
                return vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
            case PipelineStageIntent::VertexShader:
            case PipelineStageIntent::FragmentShader:
            case PipelineStageIntent::ComputeShader:
                return is_write ? vk::AccessFlagBits2::eShaderWrite
                                : vk::AccessFlagBits2::eShaderRead;
            case PipelineStageIntent::IndexInput:
                return is_write ? vk::AccessFlagBits2::eShaderWrite
                                : (vk::AccessFlagBits2::eIndexRead |
                                   vk::AccessFlagBits2::eShaderRead);
            case PipelineStageIntent::IndirectDraw:
                return vk::AccessFlagBits2::eIndirectCommandRead;
            case PipelineStageIntent::Present:
            default:
                return {};
        }
    };

    return stage_access(stage, access == AccessIntent::Write || access == AccessIntent::ReadWrite);
}

inline vk::ImageLayout IntentToImageLayout(ImageLayoutIntent intent) {
    switch (intent) {
        case ImageLayoutIntent::Undefined:
            return vk::ImageLayout::eUndefined;
        case ImageLayoutIntent::General:
            return vk::ImageLayout::eGeneral;
        case ImageLayoutIntent::ColorAttachment:
            return vk::ImageLayout::eColorAttachmentOptimal;
        case ImageLayoutIntent::DepthAttachment:
            return vk::ImageLayout::eDepthAttachmentOptimal;
        case ImageLayoutIntent::ShaderReadOnly:
            return vk::ImageLayout::eShaderReadOnlyOptimal;
        case ImageLayoutIntent::TransferSource:
            return vk::ImageLayout::eTransferSrcOptimal;
        case ImageLayoutIntent::TransferDestination:
            return vk::ImageLayout::eTransferDstOptimal;
        case ImageLayoutIntent::Present:
            return vk::ImageLayout::ePresentSrcKHR;
        case ImageLayoutIntent::DepthReadOnly:
            return vk::ImageLayout::eDepthReadOnlyOptimal;
    }
    return vk::ImageLayout::eUndefined;
}

inline vk::ImageAspectFlags FormatToAspectFlags(vk::Format format) {
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

inline vk::ImageSubresourceRange DefaultImageSubresourceRange(vk::Format format) {
    return {FormatToAspectFlags(format), 0, vk::RemainingMipLevels, 0, vk::RemainingArrayLayers};
}

// Pure barrier planning: translates the compiler-resolved transitions into
// concrete barriers with no device access. `resolved` supplies the handles;
// unresolved handles yield no image barrier / a conservative global memory
// barrier for buffers. `aliases` is reserved for Phase 2.
inline BarrierPlan PlanBarriers(const CompiledRenderGraph& graph,
                                const ResolvedResourceHandles& resolved,
                                const AliasIntervals& aliases) {
    (void)aliases;
    BarrierPlan plan{};
    plan.valid = graph.success;
    plan.passes.reserve(graph.passes.size());

    const auto image_range = [&](std::uint32_t index) {
        if (index < resolved.formats.size()) {
            return DefaultImageSubresourceRange(resolved.formats[index]);
        }
        return DefaultImageSubresourceRange(vk::Format::eUndefined);
    };

    for (std::size_t pass_index = 0; pass_index < graph.passes.size(); ++pass_index) {
        const auto& pass = graph.passes[pass_index];
        PlannedPassBarriers planned{};
        planned.pass_index = static_cast<std::uint32_t>(pass_index);
        planned.pass_name = pass.name;

        const auto emit = [&](const std::vector<ResourceTransition>& transitions,
                              std::vector<PlannedImageBarrier>& image_out,
                              std::vector<PlannedBufferBarrier>& buffer_out) {
            for (const auto& transition : transitions) {
                const std::uint32_t index = transition.resource_index;
                if (index >= graph.resource_info.size()) {
                    continue;
                }

                if (graph.resource_info[index].kind == ResourceKind::Image) {
                    PlannedImageBarrier barrier{};
                    barrier.resource_index = index;
                    if (index < resolved.images.size()) {
                        barrier.image = resolved.images[index];
                    }
                    barrier.src_stage = transition.src_stage;
                    barrier.dst_stage = transition.dst_stage;
                    barrier.src_access = transition.src_access;
                    barrier.dst_access = transition.dst_access;
                    barrier.old_layout = IntentToImageLayout(transition.from_state.layout);
                    barrier.new_layout = IntentToImageLayout(transition.target_state.layout);
                    barrier.range = image_range(index);
                    image_out.push_back(barrier);
                } else {
                    PlannedBufferBarrier barrier{};
                    barrier.resource_index = index;
                    if (index < resolved.buffers.size()) {
                        barrier.buffer = resolved.buffers[index];
                    }
                    barrier.src_stage = transition.src_stage;
                    barrier.dst_stage = transition.dst_stage;
                    barrier.src_access = transition.src_access;
                    barrier.dst_access = transition.dst_access;
                    buffer_out.push_back(barrier);
                }
            }
        };

        emit(pass.pre_pass_transitions, planned.pre_image, planned.pre_buffer);
        emit(pass.post_pass_transitions, planned.post_image, planned.post_buffer);
        plan.passes.push_back(std::move(planned));
    }

    return plan;
}

}  // namespace VulkanEngine::RenderGraph
