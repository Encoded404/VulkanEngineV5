module;

#include <cassert> // NOLINT(misc-include-cleaner)
#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)

export module VulkanEngine.PipelinePass;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanEngine.RenderGraph;
export import VulkanBackend.Vulkan.VulkanBootstrap;
export import VulkanEngine.TechniqueManager;
export import VulkanEngine.BindlessManager;
export import VulkanEngine.DescriptorDecl;

export namespace VulkanEngine::PipelinePass {

// ── Built-in pass ordering points ──
// Exhaustive: every real built-in pass, so an application can order around any
// of them. `Count` is the array size for anchors.
enum class BuiltinPass : std::uint8_t {
    Expand,
    OccluderSelect,
    OccluderPrepass,
    HiZGenPre,
    PreCull,
    DepthPrepass,
    HiZGen,
    Occlusion,
    Collect,
    MainPass,
    ImGui,
    Count,
};

inline constexpr std::size_t kBuiltinPassCount = static_cast<std::size_t>(BuiltinPass::Count);

// ── Opaque typed handles for FrameContext ──
struct BindlessTextureSet  { vk::DescriptorSet handle = nullptr; }; // NOLINT(misc-non-private-member-variables-in-classes)
struct SubmeshVertexSet    { vk::DescriptorSet handle = nullptr; }; // NOLINT(misc-non-private-member-variables-in-classes)
struct RawVertexArray      { vk::DescriptorSet handle = nullptr; }; // NOLINT(misc-non-private-member-variables-in-classes)
struct IndirectionSet      { vk::DescriptorSet handle = nullptr; }; // NOLINT(misc-non-private-member-variables-in-classes)
struct SceneUniformSet     { vk::DescriptorSet handle = nullptr; }; // NOLINT(misc-non-private-member-variables-in-classes)
struct DepthPyramid        { vk::Image image = nullptr; vk::ImageView view = nullptr; }; // NOLINT(misc-non-private-member-variables-in-classes)
struct DepthBufferView     { vk::ImageView view = nullptr; }; // NOLINT(misc-non-private-member-variables-in-classes)

// ── PassResource — a resolved resource handle for a pass ──
// Carries the Vulkan handles resolved for the current frame, keyed by resource
// identity. Typed accessors read the field matching the resource kind; raw()
// exposes everything for advanced use.
struct ResolvedResource {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    VulkanEngine::RenderGraph::ResourceHandle handle{};
    VulkanEngine::RenderGraph::ResourceKind kind = VulkanEngine::RenderGraph::ResourceKind::Image;
    vk::Image image = nullptr;
    vk::ImageView view = nullptr;
    vk::Buffer buffer = nullptr;
    vk::DeviceSize offset = 0;
    vk::DeviceSize size = vk::WholeSize;
    vk::Format format = vk::Format::eUndefined;
    bool resolved = false;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

class PassResource {
public:
    PassResource() = default;
    explicit PassResource(ResolvedResource resource) : resource_(std::move(resource)) {}

    [[nodiscard]] bool IsValid() const { return resource_.resolved; }
    [[nodiscard]] bool IsImage() const { return IsValid() && resource_.kind == VulkanEngine::RenderGraph::ResourceKind::Image; }
    [[nodiscard]] bool IsBuffer() const { return IsValid() && resource_.kind == VulkanEngine::RenderGraph::ResourceKind::Buffer; }
    [[nodiscard]] vk::Image AsImage() const { return IsImage() ? resource_.image : nullptr; }
    [[nodiscard]] vk::ImageView AsImageView() const { return IsImage() ? resource_.view : nullptr; }
    [[nodiscard]] vk::Buffer AsBuffer() const { return IsBuffer() ? resource_.buffer : nullptr; }
    [[nodiscard]] vk::DeviceSize GetOffset() const { return resource_.offset; }
    [[nodiscard]] vk::DeviceSize GetSize() const { return resource_.size; }
    [[nodiscard]] vk::Format GetFormat() const { return resource_.format; }
    [[nodiscard]] VulkanEngine::RenderGraph::ResourceHandle GetHandle() const { return resource_.handle; }
    [[nodiscard]] const ResolvedResource& raw() const { return resource_; }

private:
    ResolvedResource resource_{};
};

enum class ResourceLookupError : std::uint8_t {
    NoLookupBound,
    NotFound,
};

// Maps a resource name to the handles resolved for this frame.
class IResourceLookup {
public:
    virtual ~IResourceLookup() = default;
    [[nodiscard]] virtual std::optional<PassResource> Find(std::string_view name) const = 0;
};

// Concrete lookup the pipeline fills each frame.
class ResourceLookupTable final : public IResourceLookup {
public:
    void Clear() { entries_.clear(); }
    void Set(std::string name, ResolvedResource resource) { entries_[std::move(name)] = std::move(resource); }
    [[nodiscard]] std::optional<PassResource> Find(std::string_view name) const override {
        const auto it = entries_.find(std::string(name));
        if (it == entries_.end()) {
            return std::nullopt;
        }
        return PassResource{it->second};
    }
    [[nodiscard]] std::size_t Size() const { return entries_.size(); }

private:
    std::unordered_map<std::string, ResolvedResource> entries_{};
};

enum class PushConstantError : std::uint8_t {
    NotDeclared,
    SizeMismatch,
    NoPipelineLayout,
};

// ── Binding helpers for PassResource ──
enum class BindingError : std::uint8_t {
    NotResolved,
    NotABuffer,
    NotAnImage,
    OffsetMisaligned,
    NoImageView,
};

// Alignment limits the caller reads from the device (pure inputs here so the
// rules stay unit-testable without a device).
struct DescriptorAlignment {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint64_t min_storage_buffer_offset_alignment = 1;
    std::uint64_t min_uniform_buffer_offset_alignment = 1;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

[[nodiscard]] inline std::expected<vk::DescriptorBufferInfo, BindingError> MakeBufferBinding(
    const PassResource& resource, std::uint64_t min_offset_alignment) {
    if (!resource.IsValid()) {
        return std::unexpected(BindingError::NotResolved);
    }
    if (!resource.IsBuffer()) {
        return std::unexpected(BindingError::NotABuffer);
    }
    if (min_offset_alignment > 1 && (resource.GetOffset() % min_offset_alignment) != 0) {
        return std::unexpected(BindingError::OffsetMisaligned);
    }
    vk::DescriptorBufferInfo info{};
    info.buffer = resource.AsBuffer();
    info.offset = resource.GetOffset();
    info.range = resource.GetSize();
    return info;
}

[[nodiscard]] inline std::expected<vk::DescriptorBufferInfo, BindingError> MakeStorageBufferBinding(
    const PassResource& resource, const DescriptorAlignment& alignment) {
    return MakeBufferBinding(resource, alignment.min_storage_buffer_offset_alignment);
}

[[nodiscard]] inline std::expected<vk::DescriptorBufferInfo, BindingError> MakeUniformBufferBinding(
    const PassResource& resource, const DescriptorAlignment& alignment) {
    return MakeBufferBinding(resource, alignment.min_uniform_buffer_offset_alignment);
}

// Sampled/combined image bindings require a view; a resource resolved to an
// image without a view is an error rather than a null descriptor.
[[nodiscard]] inline std::expected<vk::DescriptorImageInfo, BindingError> MakeSampledImageBinding(
    const PassResource& resource, vk::Sampler sampler = nullptr) {
    if (!resource.IsValid()) {
        return std::unexpected(BindingError::NotResolved);
    }
    if (!resource.IsImage()) {
        return std::unexpected(BindingError::NotAnImage);
    }
    if (resource.AsImageView() == nullptr) {
        return std::unexpected(BindingError::NoImageView);
    }
    vk::DescriptorImageInfo info{};
    info.sampler = sampler;
    info.imageView = resource.AsImageView();
    info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    return info;
}

// ── Pass pipeline request — declarative, engine-owned pipeline creation ──
enum class PassPipelineKind : std::uint8_t {
    None,
    Graphics,
    Compute,
};

struct PassPipelineRequest {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    PassPipelineKind kind = PassPipelineKind::None;
    std::uint64_t vertex_shader = 0;
    std::uint64_t fragment_shader = 0;
    std::uint64_t compute_shader = 0;
    std::vector<vk::Format> color_formats{};
    vk::Format depth_format = vk::Format::eUndefined;
    std::uint32_t push_constant_size = 0;
    vk::ShaderStageFlags push_constant_stages{};
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool IsDeclared() const { return kind != PassPipelineKind::None; }
};


// ── TransientImageDesc — description of a transient (pass-owned) image ──
struct TransientImageDesc {
    std::string name{}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::Format format = vk::Format::eUndefined; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t width = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t height = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::ImageLayout initial_layout = vk::ImageLayout::eUndefined; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::ImageLayout final_layout = vk::ImageLayout::eUndefined; // NOLINT(misc-non-private-member-variables-in-classes)
    // Aliasable images are created with VK_IMAGE_CREATE_ALIAS_BIT and must use
    // an Undefined initial layout (ContentsUndefined contract).
    bool aliasable = false; // NOLINT(misc-non-private-member-variables-in-classes)
};

// ── TransientBufferDesc — description of a transient (pass-owned) buffer ──
struct TransientBufferDesc {
    std::string name{}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::DeviceSize size = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eStorageBuffer; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::MemoryPropertyFlags memory_properties = vk::MemoryPropertyFlagBits::eDeviceLocal; // NOLINT(misc-non-private-member-variables-in-classes)
    bool aliasable = false; // NOLINT(misc-non-private-member-variables-in-classes)
};

// ── IResourceRegistry — abstract interface for resource registration ──
// Implemented by RenderPipeline to allow PassSetupContext to declare
// resources without depending on the RenderPipeline concrete type.
class IResourceRegistry {
public:
    virtual ~IResourceRegistry() = default;

    virtual VulkanEngine::RenderGraph::ResourceHandle ImportDepthBuffer() = 0;
    virtual VulkanEngine::RenderGraph::ResourceHandle ImportBackbuffer() = 0;
    virtual VulkanEngine::RenderGraph::ResourceHandle ImportImage(const std::string& name) = 0;
    virtual VulkanEngine::RenderGraph::ResourceHandle ImportBuffer(const std::string& name) = 0;
    virtual VulkanEngine::RenderGraph::ResourceHandle CreateTransientImage(const TransientImageDesc& desc) = 0;
    virtual VulkanEngine::RenderGraph::ResourceHandle CreateTransientBuffer(const TransientBufferDesc& desc) = 0;
};

// ── Forward declarations ──
class IPipelinePass;
class PassSetupContext;

// ── FrameContext — per-frame resources passed to IPipelinePass::Execute() ──
struct FrameContext {
    //NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    vk::Extent2D render_extent{};
    std::uint32_t frame_index = 0;
    // Frame-in-flight ring slot (frame_index % frames_in_flight): the index
    // every per-slot resource must use.
    std::uint32_t ring_index = 0;
    std::uint32_t swapchain_image_index = 0;

    // Camera data (populated by the renderer before dispatching passes)
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::mat4 view_proj{1.0f};

    // Engine-standard descriptor sets (sets 0-4)
    BindlessTextureSet bindless_textures{};
    SubmeshVertexSet   submesh_vertices{};
    RawVertexArray      raw_vertex_buffers{};
    IndirectionSet      indirection_data{};
    SceneUniformSet     scene_uniforms{};

    // Common GPU resources
    DepthPyramid    depth_pyramid{};
    DepthBufferView depth_buffer{};

    // Scene-level data
    VulkanEngine::TechniqueManager::TechniqueManager* techniques = nullptr;
    VulkanEngine::BindlessManager::BindlessManager* bindless = nullptr;
    vk::Buffer technique_draw_commands_buffer = nullptr;
    std::uint32_t entity_count = 0;
    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;

    // Per-frame resource resolution, set by the pipeline before execution.
    const IResourceLookup* resource_lookup = nullptr;

    // Pipeline layout for push constants (set per-pass by PassSetupContext)
    vk::PipelineLayout pipeline_layout = nullptr;

    // Push constant metadata for runtime validation
    std::uint32_t declared_push_constant_size = 0;
    vk::ShaderStageFlags declared_push_constant_stages{};
    //NOLINTEND(misc-non-private-member-variables-in-classes)

    // ── Push constant upload with runtime validation ──
    [[nodiscard]] std::expected<void, PushConstantError> ValidatePushConstants(std::size_t type_size) const {
        if (declared_push_constant_stages == vk::ShaderStageFlags{}) {
            return std::unexpected(PushConstantError::NotDeclared);
        }
        if (declared_push_constant_size != type_size) {
            return std::unexpected(PushConstantError::SizeMismatch);
        }
        if (pipeline_layout == nullptr) {
            return std::unexpected(PushConstantError::NoPipelineLayout);
        }
        return {};
    }

    template<typename T>
    [[nodiscard]] std::expected<void, PushConstantError> SetPushConstants(vk::CommandBuffer cmd, const T& src) const {
        auto valid = ValidatePushConstants(sizeof(T));
        if (!valid.has_value()) {
            return valid;
        }
        cmd.pushConstants(pipeline_layout, declared_push_constant_stages, 0, sizeof(T), &src);
        return {};
    }

    // ── Resolve a resource declared in Setup() ──
    // Looks the name up in the per-frame identity table.
    [[nodiscard]] std::expected<PassResource, ResourceLookupError> GetResource(std::string_view name) const {
        if (resource_lookup == nullptr) {
            return std::unexpected(ResourceLookupError::NoLookupBound);
        }
        auto found = resource_lookup->Find(name);
        if (!found.has_value()) {
            return std::unexpected(ResourceLookupError::NotFound);
        }
        return *found;
    }
};

// The executor's single opaque user_data. Built-in pass lambdas read
// `engine_user_data` (the renderer's frame context); custom passes read the
// populated `frame`. Phase 7 migrates built-ins to `frame` without another
// plumbing change.
struct RenderFrameData {
    FrameContext frame{};
    const void* engine_user_data = nullptr;
};

// ── PassSetupContext — resource and ordering declarations in Setup() ──
class PassSetupContext {
public:
    explicit PassSetupContext(IResourceRegistry& registry,
                              std::uint32_t render_width = 0,
                              std::uint32_t render_height = 0);
    ~PassSetupContext() = default;

    PassSetupContext(const PassSetupContext&) = delete;
    PassSetupContext& operator=(const PassSetupContext&) = delete;
    PassSetupContext(PassSetupContext&&) = default;
    PassSetupContext& operator=(PassSetupContext&&) = default;

    // ── Ordering relative to the fixed built-in pipeline ──
    void RunBefore(BuiltinPass pass);
    void RunAfter(BuiltinPass pass);

    // ── GPU resource imports and transient creation ──
    VulkanEngine::RenderGraph::ResourceHandle ReadDepthBuffer();
    VulkanEngine::RenderGraph::ResourceHandle ReadBackbuffer();
    VulkanEngine::RenderGraph::ResourceHandle ImportImage(std::string_view name);
    VulkanEngine::RenderGraph::ResourceHandle ImportBuffer(std::string_view name);
    VulkanEngine::RenderGraph::ResourceHandle CreateTransientImage(const TransientImageDesc& desc);
    VulkanEngine::RenderGraph::ResourceHandle CreateTransientBuffer(const TransientBufferDesc& desc);

    // ── Render graph resource usage declarations ──
    void AddRead(VulkanEngine::RenderGraph::ResourceHandle res,
                 VulkanEngine::RenderGraph::PipelineStageIntent stage,
                 VulkanEngine::RenderGraph::AccessIntent access);
    void AddWrite(VulkanEngine::RenderGraph::ResourceHandle res);

    // ── Attachment pass declaration ──
    void SetPassAttachments(VulkanEngine::RenderGraph::PassAttachmentSetup setup);

    // ── Typed push constant declaration ──
    template<typename T>
    void DeclarePushConstants(vk::ShaderStageFlags stages) {
        push_constant_size_ = static_cast<std::uint32_t>(sizeof(T));
        push_constant_stages_ = stages;
    }

    // ── Engine-owned pipeline declaration ──
    // The engine creates/owns the pipeline layout and pipeline, and hot-reloads
    // it; the pass never touches VkPipeline directly.
    void RequestGraphicsPipeline(std::uint64_t vertex_shader, std::uint64_t fragment_shader,
                                 std::vector<vk::Format> color_formats = {},
                                 vk::Format depth_format = vk::Format::eUndefined);
    void RequestComputePipeline(std::uint64_t compute_shader);
    [[nodiscard]] const PassPipelineRequest& GetPipelineRequest() const { return pipeline_request_; }

    // ── Descriptor declaration (app sets >= 5) ──
    void DeclareBindings(std::vector<VulkanEngine::Render::DescriptorDecl> bindings);
    [[nodiscard]] const std::vector<VulkanEngine::Render::DescriptorDecl>& GetDeclaredBindings() const { return declared_bindings_; }

    // ── Render extent query ──
    [[nodiscard]] std::uint32_t GetRenderWidth() const;
    [[nodiscard]] std::uint32_t GetRenderHeight() const;

    // ── Internal: accessors for RenderPipeline integration ──
    [[nodiscard]] const std::vector<VulkanEngine::RenderGraph::PassHandle>& GetDeferredBefore() const { return deferred_before_; }
    [[nodiscard]] const std::vector<VulkanEngine::RenderGraph::PassHandle>& GetDeferredAfter() const { return deferred_after_; }
    [[nodiscard]] std::uint32_t GetPushConstantSize() const { return push_constant_size_; }
    [[nodiscard]] vk::ShaderStageFlags GetPushConstantStages() const { return push_constant_stages_; }

    // ── Resource declaration accessors (populated during Setup()) ──
    [[nodiscard]] const std::vector<BuiltinPass>& GetBeforeBuiltinPasses() const { return before_builtin_passes_; }
    [[nodiscard]] const std::vector<BuiltinPass>& GetAfterBuiltinPasses() const { return after_builtin_passes_; }
    [[nodiscard]] const std::vector<VulkanEngine::RenderGraph::ResourceHandle>& GetReadResources() const { return read_resources_; }
    [[nodiscard]] const std::vector<VulkanEngine::RenderGraph::PipelineStageIntent>& GetReadStages() const { return read_stages_; }
    [[nodiscard]] const std::vector<VulkanEngine::RenderGraph::AccessIntent>& GetReadAccesses() const { return read_accesses_; }
    [[nodiscard]] const std::vector<VulkanEngine::RenderGraph::ResourceHandle>& GetWriteResources() const { return write_resources_; }
    [[nodiscard]] const std::optional<VulkanEngine::RenderGraph::PassAttachmentSetup>& GetAttachmentSetup() const { return attachment_setup_; }

private:
    friend class IPipelinePass;

    IResourceRegistry* registry_ = nullptr;
    VulkanEngine::RenderGraph::PassHandle pass_handle_{};
    bool pass_handle_assigned_ = false;

    // Deferred dependencies (resolved when pass handle is assigned)
    std::vector<VulkanEngine::RenderGraph::PassHandle> deferred_before_;
    std::vector<VulkanEngine::RenderGraph::PassHandle> deferred_after_;

    // Builtin pass ordering (stored as enums, resolved in AddCustomPass)
    std::vector<BuiltinPass> before_builtin_passes_;
    std::vector<BuiltinPass> after_builtin_passes_;

    // Resource read/write declarations
    std::vector<VulkanEngine::RenderGraph::ResourceHandle> read_resources_;
    std::vector<VulkanEngine::RenderGraph::PipelineStageIntent> read_stages_;
    std::vector<VulkanEngine::RenderGraph::AccessIntent> read_accesses_;
    std::vector<VulkanEngine::RenderGraph::ResourceHandle> write_resources_;

    // Attachment setup
    std::optional<VulkanEngine::RenderGraph::PassAttachmentSetup> attachment_setup_;

    // Push constant declaration
    std::uint32_t push_constant_size_ = 0;
    vk::ShaderStageFlags push_constant_stages_{};

    // Engine-owned pipeline + app descriptor declaration
    PassPipelineRequest pipeline_request_{};
    std::vector<VulkanEngine::Render::DescriptorDecl> declared_bindings_{};

    std::uint32_t render_width_ = 0;
    std::uint32_t render_height_ = 0;
};

// ── IPipelinePass — abstract base for all pipeline passes ──
//
// NOTE: Pass classes currently expose two Execute() overloads:
//   1. A legacy execute(cmd, params...) taking explicit Vulkan handles,
//      called directly by SceneRenderer::Dispatch*() methods.
//   2. Execute(const FrameContext&, vk::CommandBuffer) — the IPipelinePass
//      override, which delegates to SceneRenderer::Dispatch*().
//
// TODO(cleanup): Once the render graph fully takes over pass dispatch,
// the legacy execute() methods should be made private (friend SceneRenderer)
// or removed entirely in favor of FrameContext-driven execution.
class IPipelinePass {
public:
    virtual ~IPipelinePass() = default;

    // Declare GPU resources and ordering during pipeline setup
    virtual void Setup(PassSetupContext& ctx) = 0;

    // Execute the pass every frame
    virtual void Execute(const FrameContext& ctx, vk::CommandBuffer cmd) = 0;

    // Optional: validate configuration before compilation
    [[nodiscard]] virtual bool Validate() const { return true; }

    // Stable, human-readable name used for graph diagnostics, ordering, and
    // duplicate-name validation. Override for application passes.
    [[nodiscard]] virtual std::string_view GetName() const { return "AppPass"; }
};

} // namespace VulkanEngine::PipelinePass
