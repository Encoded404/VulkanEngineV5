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
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;

    [[nodiscard]] bool IsValid() const noexcept {
        return index != std::numeric_limits<std::uint32_t>::max();
    }

    friend bool operator==(const ResourceHandle&, const ResourceHandle&) = default;
};

struct PassHandle {
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;

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
};

struct TransientBufferInfo {
    vk::DeviceSize size = 0;
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eStorageBuffer;
    vk::MemoryPropertyFlags memory_properties = vk::MemoryPropertyFlagBits::eDeviceLocal;
};

// ── Barrier and transition types ──

struct ImageBarrier {
    vk::PipelineStageFlags src_stage = vk::PipelineStageFlagBits::eTopOfPipe;
    vk::PipelineStageFlags dst_stage = vk::PipelineStageFlagBits::eTopOfPipe;
    vk::AccessFlags src_access = {};
    vk::AccessFlags dst_access = {};
    vk::ImageLayout old_layout = vk::ImageLayout::eUndefined;
    vk::ImageLayout new_layout = vk::ImageLayout::eUndefined;
    vk::Image image = {};
    std::uint32_t resource_index = std::numeric_limits<std::uint32_t>::max();
    vk::ImageSubresourceRange subresource_range = {};
    std::uint32_t src_queue_family = vk::QueueFamilyIgnored;
    std::uint32_t dst_queue_family = vk::QueueFamilyIgnored;
};

struct BufferBarrier {
    vk::PipelineStageFlags src_stage = vk::PipelineStageFlagBits::eTopOfPipe;
    vk::PipelineStageFlags dst_stage = vk::PipelineStageFlagBits::eTopOfPipe;
    vk::AccessFlags src_access = {};
    vk::AccessFlags dst_access = {};
    std::uint32_t resource_index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t src_queue_family = vk::QueueFamilyIgnored;
    std::uint32_t dst_queue_family = vk::QueueFamilyIgnored;
};

struct ResourceTransition {
    std::uint32_t resource_index = std::numeric_limits<std::uint32_t>::max();
    ResourceState target_state{};
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

// ── Pass execution ──

struct PassExecutionCallback {
    std::function<void(const void* user_data, vk::CommandBuffer command_buffer)> callback{};
};

// ── Resource metadata ──

struct ResourceInfo {
    std::string name{};
    ResourceKind kind = ResourceKind::Image;
    bool imported = false;
    std::optional<TransientImageInfo> image_info{};
    std::optional<TransientBufferInfo> buffer_info{};
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
    bool success = false;
    std::vector<CompileDiagnostic> diagnostics{};
    std::vector<CompiledPass> passes{};
    std::vector<ResourceLifetime> resource_lifetimes{};
    std::vector<ResourceInfo> resource_info{};
    mutable std::vector<ResourceState> initial_states{};
    mutable std::vector<bool> has_initial_state{};
    std::vector<vk::Image> resource_images{};
    std::vector<vk::Format> resource_formats{};

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

    void SetResourceFormat(std::uint32_t resource_index, vk::Format format) {
        if (resource_index < resource_formats.size()) {
            resource_formats[resource_index] = format;
        }
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

inline vk::PipelineStageFlags IntentToPipelineStage(PipelineStageIntent intent, AccessIntent access) {
    switch (intent) {
        case PipelineStageIntent::None:
            return {};
        case PipelineStageIntent::Transfer:
            return vk::PipelineStageFlagBits::eTransfer;
        case PipelineStageIntent::ColorAttachment:
            return vk::PipelineStageFlagBits::eColorAttachmentOutput;
        case PipelineStageIntent::DepthAttachment:
            return vk::PipelineStageFlagBits::eEarlyFragmentTests |
                   vk::PipelineStageFlagBits::eLateFragmentTests;
        case PipelineStageIntent::VertexShader:
            return vk::PipelineStageFlagBits::eVertexShader;
        case PipelineStageIntent::FragmentShader:
            return vk::PipelineStageFlagBits::eFragmentShader;
        case PipelineStageIntent::ComputeShader:
            return vk::PipelineStageFlagBits::eComputeShader;
        case PipelineStageIntent::IndirectDraw:
            return vk::PipelineStageFlagBits::eDrawIndirect;
        case PipelineStageIntent::Present:
            return vk::PipelineStageFlagBits::eBottomOfPipe;
        case PipelineStageIntent::TopOfPipe:
            return vk::PipelineStageFlagBits::eTopOfPipe;
        case PipelineStageIntent::BottomOfPipe:
            return vk::PipelineStageFlagBits::eBottomOfPipe;
    }
    return vk::PipelineStageFlagBits::eTopOfPipe;
}

inline vk::AccessFlags IntentToAccessFlags(PipelineStageIntent stage, AccessIntent access) {
    if (access == AccessIntent::None) return {};

    auto stage_access = [](PipelineStageIntent s, bool is_write) -> vk::AccessFlags {
        switch (s) {
            case PipelineStageIntent::Transfer:
                return is_write ? vk::AccessFlagBits::eTransferWrite
                                : vk::AccessFlagBits::eTransferRead;
            case PipelineStageIntent::ColorAttachment:
                return vk::AccessFlagBits::eColorAttachmentWrite;
            case PipelineStageIntent::DepthAttachment:
                return vk::AccessFlagBits::eDepthStencilAttachmentWrite;
            case PipelineStageIntent::VertexShader:
                return is_write ? vk::AccessFlagBits::eShaderWrite
                                : vk::AccessFlagBits::eShaderRead;
            case PipelineStageIntent::FragmentShader:
                return is_write ? vk::AccessFlagBits::eShaderWrite
                                : vk::AccessFlagBits::eShaderRead;
            case PipelineStageIntent::ComputeShader:
                return is_write ? vk::AccessFlagBits::eShaderWrite
                                : vk::AccessFlagBits::eShaderRead;
            case PipelineStageIntent::IndirectDraw:
                return vk::AccessFlagBits::eIndirectCommandRead;
            case PipelineStageIntent::Present:
                return {};
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

}  // namespace VulkanEngine::RenderGraph
