module;

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

export module VulkanEngine.RenderGraph;

export namespace VulkanEngine::RenderGraph {

enum class PipelineStageIntent : uint8_t {
    None,
    Transfer,
    ColorAttachment,
    DepthAttachment,
    FragmentShader,
    ComputeShader,
    Present
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
    Present
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

using PassExecutionCallback = std::function<void(const void* user_data)>;

struct ExecutablePass {
    PassHandle handle{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::string name{}; // NOLINT(misc-non-private-member-variables-in-classes)
    QueueType queue = QueueType::Graphics; // NOLINT(misc-non-private-member-variables-in-classes)
    PassExecutionCallback execute{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ResourceLifetime {
    ResourceHandle handle{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::string name{}; // NOLINT(misc-non-private-member-variables-in-classes)
    bool imported = false; // NOLINT(misc-non-private-member-variables-in-classes)
    bool transient = false; // NOLINT(misc-non-private-member-variables-in-classes)
    int32_t first_pass = -1; // NOLINT(misc-non-private-member-variables-in-classes)
    int32_t last_pass = -1; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct CompileResult {
    bool success = false; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<CompileDiagnostic> diagnostics{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<PassHandle> execution_order{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<ExecutablePass> executable_passes{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<ResourceLifetime> resource_lifetimes{}; // NOLINT(misc-non-private-member-variables-in-classes)

    void Execute(const void* user_data = nullptr) const;
};

class RenderGraphBuilder {
public:
    ResourceHandle CreateTransientResource(std::string name, ResourceKind kind);
    ResourceHandle ImportResource(std::string name, ResourceKind kind);

    bool SetInitialState(ResourceHandle resource, ResourceState state);
    bool SetFinalState(ResourceHandle resource, ResourceState state);

    PassHandle AddPass(std::string name,
                       QueueType queue = QueueType::Graphics,
                       bool enabled = true,
                       PassExecutionCallback execute = {});

    bool AddRead(PassHandle pass, ResourceHandle resource);
    bool AddWrite(PassHandle pass, ResourceHandle resource);
    bool AddDependency(PassHandle before, PassHandle after);

    [[nodiscard]] CompileResult Compile() const;

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
    };

    struct PassNode {
        std::string name{};
        QueueType queue = QueueType::Graphics;
        uint32_t generation = 1;
        bool enabled = true;
        std::vector<ResourceHandle> reads{};
        std::vector<ResourceHandle> writes{};
        PassExecutionCallback execute{};
    };

    [[nodiscard]] bool IsValidResourceHandle(ResourceHandle handle) const;
    [[nodiscard]] bool IsValidPassHandle(PassHandle handle) const;

    std::vector<ResourceNode> resources_{};
    std::vector<PassNode> passes_{};
    std::vector<std::pair<PassHandle, PassHandle>> explicit_dependencies_{};
};

}  // namespace VulkanEngine::RenderGraph
