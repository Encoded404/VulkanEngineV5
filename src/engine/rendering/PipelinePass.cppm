module;

export module VulkanEngine.PipelinePass;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanEngine.RenderPipeline;
export import VulkanBackend.RenderGraph;
export import VulkanBackend.Runtime.VulkanBootstrap;

export namespace VulkanEngine::PipelinePass {

// ── Built-in pass ordering points ──
export enum class BuiltinPass : std::uint8_t {
    Expand,
    DepthPrepass,
    HiZGen,
    Occlusion,
    Collect,
    MainPass,
};

// ── Opaque typed handles for FrameContext ──
export struct BindlessTextureSet  { vk::DescriptorSet handle = nullptr; }; // NOLINT(misc-non-private-member-variables-in-classes)
export struct SubmeshVertexSet    { vk::DescriptorSet handle = nullptr; }; // NOLINT(misc-non-private-member-variables-in-classes)
export struct RawVertexArray      { vk::DescriptorSet handle = nullptr; }; // NOLINT(misc-non-private-member-variables-in-classes)
export struct IndirectionSet      { vk::DescriptorSet handle = nullptr; }; // NOLINT(misc-non-private-member-variables-in-classes)
export struct DepthPyramid        { vk::Image image = nullptr; vk::ImageView view = nullptr; }; // NOLINT(misc-non-private-member-variables-in-classes)
export struct DepthBufferView     { vk::ImageView view = nullptr; }; // NOLINT(misc-non-private-member-variables-in-classes)

// ── Forward declarations ──
class IPipelinePass;
class PassSetupContext;

// ── FrameContext — per-frame resources passed to IPipelinePass::Execute() ──
export struct FrameContext {
    vk::Extent2D render_extent{};
    std::uint32_t frame_index = 0;
    std::uint32_t swapchain_image_index = 0;

    // Engine-standard descriptor sets (sets 0-3)
    BindlessTextureSet bindless_textures{};
    SubmeshVertexSet   submesh_vertices{};
    RawVertexArray      raw_vertex_buffers{};
    IndirectionSet      indirection_data{};

    // Common GPU resources
    DepthPyramid    depth_pyramid{};
    DepthBufferView depth_buffer{};

    // Pipeline layout for push constants (set per-pass by PassSetupContext)
    vk::PipelineLayout pipeline_layout = nullptr;

    // Push constant metadata for runtime validation
    std::uint32_t declared_push_constant_size = 0;
    vk::ShaderStageFlags declared_push_constant_stages{};

    // ── Push constant upload with runtime validation ──
    template<typename T>
    void SetPushConstants(vk::CommandBuffer cmd, const T& src) const {
        assert(declared_push_constant_size == sizeof(T) &&
               "Push constant type size doesn't match declaration");
        assert(declared_push_constant_stages != vk::ShaderStageFlags{} &&
               "Push constants were never declared for this pass");
        cmd.pushConstants(pipeline_layout, declared_push_constant_stages, 0, sizeof(T), &src);
    }

    // ── Resolve a resource declared in Setup() ──
    // Returns the Vulkan handle for a resource by its name.
    // Implemented in PipelinePass.cpp via the resource map populated during compilation.
    VulkanEngine::RenderGraph::ResourceHandle GetResource(std::string_view name) const;
};

// ── PassSetupContext — resource and ordering declarations in Setup() ──
export class PassSetupContext {
public:
    explicit PassSetupContext(VulkanEngine::RenderPipeline::RenderPipeline& pipeline);
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
    VulkanEngine::RenderGraph::ResourceHandle CreateTransientImage(const VulkanEngine::RenderPipeline::TransientImageDesc& desc);

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

    // ── Render extent query ──
    std::uint32_t GetRenderWidth() const;
    std::uint32_t GetRenderHeight() const;

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

    VulkanEngine::RenderPipeline::RenderPipeline* pipeline_ = nullptr;
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

    std::uint32_t render_width_ = 0;
    std::uint32_t render_height_ = 0;
};

// ── IPipelinePass — abstract base for all pipeline passes ──
export class IPipelinePass {
public:
    virtual ~IPipelinePass() = default;

    // Declare GPU resources and ordering during pipeline setup
    virtual void Setup(PassSetupContext& ctx) = 0;

    // Execute the pass every frame
    virtual void Execute(const FrameContext& ctx, vk::CommandBuffer cmd) = 0;

    // Optional: validate configuration before compilation
    virtual bool Validate() const { return true; }
};

} // namespace VulkanEngine::PipelinePass
