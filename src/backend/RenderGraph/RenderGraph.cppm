module;

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.hpp>

export module VulkanBackend.RenderGraph;

export namespace VulkanEngine::RenderGraph {

enum class PipelineStageIntent : uint8_t {
    None,
    Transfer,
    ColorAttachment,
    DepthAttachment,
    FragmentShader,
    ComputeShader,
    Present,
    TopOfPipe,
    BottomOfPipe
};

enum class AccessIntent : uint8_t {
    None,
    Read,
    Write,
    ReadWrite
};

enum class ImageLayoutIntent : uint8_t {
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

enum class QueueType : uint8_t {
    Graphics,
    Compute,
    Transfer
};

struct ResourceState {
    PipelineStageIntent stage = PipelineStageIntent::None; // NOLINT(misc-non-private-member-variables-in-classes)
    AccessIntent access = AccessIntent::None; // NOLINT(misc-non-private-member-variables-in-classes)
    QueueType queue = QueueType::Graphics; // NOLINT(misc-non-private-member-variables-in-classes)
    ImageLayoutIntent layout = ImageLayoutIntent::Undefined; // NOLINT(misc-non-private-member-variables-in-classes)
    bool has_image_layout = false; // NOLINT(misc-non-private-member-variables-in-classes)

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

enum class ResourceKind : uint8_t {
    Image,
    Buffer
};

struct ResourceHandle {
    uint32_t index = UINT32_MAX; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t generation = 0; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool IsValid() const noexcept {
        return index != UINT32_MAX;
    }

    friend bool operator==(const ResourceHandle&, const ResourceHandle&) = default;
};

struct PassHandle {
    uint32_t index = UINT32_MAX; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t generation = 0; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool IsValid() const noexcept {
        return index != UINT32_MAX;
    }

    friend bool operator==(const PassHandle&, const PassHandle&) = default;
};

enum class DiagnosticSeverity : uint8_t {
    Info,
    Warning,
    Error
};

enum class DiagnosticCode : uint8_t {
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
    DiagnosticCode code = DiagnosticCode::None; // NOLINT(misc-non-private-member-variables-in-classes)
    DiagnosticSeverity severity = DiagnosticSeverity::Info; // NOLINT(misc-non-private-member-variables-in-classes)
    std::string message{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct TransientImageInfo {
    vk::Format format = vk::Format::eUndefined; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t width = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t height = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t mip_levels = 1; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t array_layers = 1; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::SampleCountFlagBits sample_count = vk::SampleCountFlagBits::e1; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::ImageTiling tiling = vk::ImageTiling::eOptimal; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct TransientBufferInfo {
    vk::DeviceSize size = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eStorageBuffer; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::MemoryPropertyFlags memory_properties = vk::MemoryPropertyFlagBits::eDeviceLocal; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ImageBarrier {
    vk::PipelineStageFlags src_stage = vk::PipelineStageFlagBits::eTopOfPipe; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::PipelineStageFlags dst_stage = vk::PipelineStageFlagBits::eTopOfPipe; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::AccessFlags src_access = {}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::AccessFlags dst_access = {}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::ImageLayout old_layout = vk::ImageLayout::eUndefined; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::ImageLayout new_layout = vk::ImageLayout::eUndefined; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::Image image = {}; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t resource_index = UINT32_MAX; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::ImageSubresourceRange subresource_range = {}; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t src_queue_family = VK_QUEUE_FAMILY_IGNORED; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t dst_queue_family = VK_QUEUE_FAMILY_IGNORED; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct BufferBarrier {
    vk::PipelineStageFlags src_stage = vk::PipelineStageFlagBits::eTopOfPipe; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::PipelineStageFlags dst_stage = vk::PipelineStageFlagBits::eTopOfPipe; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::AccessFlags src_access = {}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::AccessFlags dst_access = {}; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t resource_index = UINT32_MAX; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t src_queue_family = VK_QUEUE_FAMILY_IGNORED; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t dst_queue_family = VK_QUEUE_FAMILY_IGNORED; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct AttachmentInfo {
    ResourceHandle resource{}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::ImageView image_view = {}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::AttachmentLoadOp load_op = vk::AttachmentLoadOp::eClear; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::AttachmentStoreOp store_op = vk::AttachmentStoreOp::eStore; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::ClearColorValue clear_color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}); // NOLINT(misc-non-private-member-variables-in-classes)
    vk::ClearDepthStencilValue clear_depth = vk::ClearDepthStencilValue(1.0f, 0); // NOLINT(misc-non-private-member-variables-in-classes)
};

struct PassAttachmentSetup {
    std::vector<AttachmentInfo> color_attachments{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::optional<AttachmentInfo> depth_attachment{}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::Rect2D render_area = {}; // NOLINT(misc-non-private-member-variables-in-classes)
    bool auto_begin_rendering = false; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct PassExecutionCallback {
    std::function<void(const void* user_data, vk::CommandBuffer command_buffer)> callback{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ResourceTransition {
    uint32_t resource_index = UINT32_MAX; // NOLINT(misc-non-private-member-variables-in-classes)
    ResourceState target_state{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ResourceInfo {
    std::string name{}; // NOLINT(misc-non-private-member-variables-in-classes)
    ResourceKind kind = ResourceKind::Image; // NOLINT(misc-non-private-member-variables-in-classes)
    bool imported = false; // NOLINT(misc-non-private-member-variables-in-classes)
    std::optional<TransientImageInfo> image_info{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::optional<TransientBufferInfo> buffer_info{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct CompiledPass {
    PassHandle handle{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::string name{}; // NOLINT(misc-non-private-member-variables-in-classes)
    QueueType queue = QueueType::Graphics; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<ResourceTransition> pre_pass_transitions{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<ResourceTransition> post_pass_transitions{}; // NOLINT(misc-non-private-member-variables-in-classes)
    PassExecutionCallback execute{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::optional<PassAttachmentSetup> attachment_setup{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ResourceLifetime {
    ResourceHandle handle{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::string name{}; // NOLINT(misc-non-private-member-variables-in-classes)
    bool imported = false; // NOLINT(misc-non-private-member-variables-in-classes)
    bool transient = false; // NOLINT(misc-non-private-member-variables-in-classes)
    int32_t first_pass = -1; // NOLINT(misc-non-private-member-variables-in-classes)
    int32_t last_pass = -1; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct CompiledRenderGraph {
    bool success = false; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<CompileDiagnostic> diagnostics{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<CompiledPass> passes{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<ResourceLifetime> resource_lifetimes{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<ResourceInfo> resource_info{}; // NOLINT(misc-non-private-member-variables-in-classes)
    mutable std::vector<ResourceState> initial_states{}; // NOLINT(misc-non-private-member-variables-in-classes)
    mutable std::vector<bool> has_initial_state{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<vk::Image> resource_images{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<vk::Format> resource_formats{}; // NOLINT(misc-non-private-member-variables-in-classes)

    void Execute(const void* user_data, vk::CommandBuffer command_buffer) const;
    void SetImportedResourceState(uint32_t resource_index, ResourceState state) const;
    void SetResourceImage(uint32_t resource_index, vk::Image image);
    void SetResourceFormat(uint32_t resource_index, vk::Format format);
};

class RenderGraphBuilder {
public:
    ResourceHandle CreateTransientResource(std::string name, ResourceKind kind);
    ResourceHandle ImportResource(std::string name, ResourceKind kind);

    bool SetTransientImageInfo(ResourceHandle resource, TransientImageInfo info);
    bool SetTransientBufferInfo(ResourceHandle resource, TransientBufferInfo info);

    bool SetInitialState(ResourceHandle resource, ResourceState state);
    bool SetFinalState(ResourceHandle resource, ResourceState state);

    PassHandle AddPass(std::string name,
                       QueueType queue = QueueType::Graphics,
                       bool enabled = true,
                       PassExecutionCallback execute = {});

    bool AddRead(PassHandle pass, ResourceHandle resource);
    bool AddWrite(PassHandle pass, ResourceHandle resource);
    bool AddDependency(PassHandle before, PassHandle after);

    bool SetPassAttachments(PassHandle pass, PassAttachmentSetup setup);

    [[nodiscard]] CompiledRenderGraph Compile() const;

private:
    struct ResourceNode {
        std::string name{};
        ResourceKind kind = ResourceKind::Image;
        uint32_t generation = 1;
        bool imported = false;
        bool transient = false;
        bool has_initial_state = false;
        bool has_final_state = false;
        ResourceState initial_state{};
        ResourceState final_state{};
        std::optional<TransientImageInfo> image_info{};
        std::optional<TransientBufferInfo> buffer_info{};
    };

    struct PassNode {
        std::string name{};
        QueueType queue = QueueType::Graphics;
        uint32_t generation = 1;
        bool enabled = true;
        std::vector<ResourceHandle> reads{};
        std::vector<ResourceHandle> writes{};
        PassExecutionCallback execute{};
        std::optional<PassAttachmentSetup> attachment_setup{};
    };

    [[nodiscard]] bool IsValidResourceHandle(ResourceHandle handle) const;
    [[nodiscard]] bool IsValidPassHandle(PassHandle handle) const;

    std::vector<ResourceNode> resources_{};
    std::vector<PassNode> passes_{};
    std::vector<std::pair<PassHandle, PassHandle>> explicit_dependencies_{};
};

}  // namespace VulkanEngine::RenderGraph (exported)

// Module-linkage helpers (visible to all implementation units, not exported)
namespace VulkanEngine::RenderGraph {

bool ContainsResource(const std::vector<ResourceHandle>& handles, ResourceHandle value);
bool ContainsDependency(const std::vector<std::pair<PassHandle, PassHandle>>& deps,
                        const std::pair<PassHandle, PassHandle>& value);
bool IsResourceStateCompatible(ResourceKind kind, const ResourceState& state);
vk::PipelineStageFlags IntentToPipelineStage(PipelineStageIntent intent, AccessIntent access);
vk::AccessFlags IntentToAccessFlags(PipelineStageIntent stage, AccessIntent access);
vk::ImageLayout IntentToImageLayout(ImageLayoutIntent intent);
vk::ImageAspectFlags FormatToAspectFlags(vk::Format format);
bool StatesEqual(const ResourceState& a, const ResourceState& b);

}  // namespace VulkanEngine::RenderGraph (helpers)
