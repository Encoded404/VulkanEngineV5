module;

export module VulkanEngine.RenderPipeline;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanEngine.RenderGraph;
export import VulkanBackend.Vulkan.VulkanBootstrap;
export import VulkanEngine.PipelinePass;

import VulkanEngine.GpuResources.TransientAllocator;
import VulkanEngine.PipelineFactory;
import VulkanEngine.ShaderManager;

export namespace VulkanEngine::RenderPipeline {

struct ReadResourceDesc {
    VulkanEngine::RenderGraph::ResourceHandle resource{};
    VulkanEngine::RenderGraph::PipelineStageIntent stage = VulkanEngine::RenderGraph::PipelineStageIntent::FragmentShader;
    VulkanEngine::RenderGraph::AccessIntent access = VulkanEngine::RenderGraph::AccessIntent::Read;
};

struct RenderPipelinePassDesc {
    std::string name{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::RenderGraph::QueueType queue = VulkanEngine::RenderGraph::QueueType::Graphics; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<ReadResourceDesc> reads{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<VulkanEngine::RenderGraph::ResourceHandle> writes{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::optional<VulkanEngine::RenderGraph::PassAttachmentSetup> attachments{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::PipelinePass::PassPipelineRequest pipeline_request{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<VulkanEngine::Render::DescriptorDecl> declared_bindings{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::function<void(const void* user_data, vk::CommandBuffer command_buffer)> execute{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

// TransientImageDesc is now provided by the VulkanEngine.PipelinePass module.
using VulkanEngine::PipelinePass::TransientImageDesc;
using VulkanEngine::PipelinePass::TransientBufferDesc;

class RenderPipeline : public VulkanEngine::PipelinePass::IResourceRegistry {
public:
    RenderPipeline();
    ~RenderPipeline() override;

    void Initialize(VulkanBackend::Vulkan::VulkanBootstrap& bootstrap,
                    ShaderSystem::ShaderManager* shader_manager = nullptr,
                    ShaderSystem::PipelineFactory* pipeline_factory = nullptr);
    void Shutdown();

    // Engine-standard descriptor set layouts (sets 0-4) used to build pass
    // pipeline layouts. Must be set before the first Compile().
    void SetEngineDescriptorSetLayouts(std::array<vk::DescriptorSetLayout, 5> layouts);

    // ── IResourceRegistry overrides ──
    VulkanEngine::RenderGraph::ResourceHandle ImportBackbuffer() override;
    VulkanEngine::RenderGraph::ResourceHandle ImportDepthBuffer() override;
    VulkanEngine::RenderGraph::ResourceHandle ImportImage(const std::string& name) override;
    VulkanEngine::RenderGraph::ResourceHandle ImportBuffer(const std::string& name) override;
    VulkanEngine::RenderGraph::ResourceHandle CreateTransientImage(const TransientImageDesc& desc) override;
    VulkanEngine::RenderGraph::ResourceHandle CreateTransientBuffer(const TransientBufferDesc& desc) override;

    using ImageResolver = std::function<vk::Image(std::uint32_t image_index)>;
    using ImageViewResolver = std::function<vk::ImageView(std::uint32_t image_index)>;
    using BufferResolver = std::function<vk::Buffer(std::uint32_t image_index)>;
    void RegisterResourceResolver(const std::string& name,
                                  ImageResolver resolve_image,
                                  ImageViewResolver resolve_image_view,
                                  vk::Format format);

    // Registers the concrete buffer backing an imported Buffer resource. Until
    // a resolver exists the planned barrier for that resource falls back to a
    // conservative global memory barrier instead of being skipped.
    void RegisterBufferResolver(const std::string& name, BufferResolver resolve_buffer);

    VulkanEngine::RenderGraph::PassHandle AddPass(const RenderPipelinePassDesc& desc);

    // ── Custom pass registration ──
    // Creates a pass from an IPipelinePass, calling Setup() to collect resources.
    // Returns the pass handle for custom-to-custom ordering via AddDependency().
    VulkanEngine::RenderGraph::PassHandle AddCustomPass(
        std::unique_ptr<VulkanEngine::PipelinePass::IPipelinePass> pass,
        VulkanEngine::PipelinePass::PassSetupContext& ctx);

    // ── Built-in pass handle access ──
    // The caller (Renderer) populates these after registering all built-in passes.
    void SetBuiltinHandles(const std::array<VulkanEngine::RenderGraph::PassHandle, 6>& handles);
    [[nodiscard]] const std::array<VulkanEngine::RenderGraph::PassHandle, 6>& GetBuiltinHandles() const;

    bool AddDependency(VulkanEngine::RenderGraph::PassHandle before,
                       VulkanEngine::RenderGraph::PassHandle after);

    bool SetInitialState(VulkanEngine::RenderGraph::ResourceHandle resource, VulkanEngine::RenderGraph::ResourceState state);
    bool SetFinalState(VulkanEngine::RenderGraph::ResourceHandle resource, VulkanEngine::RenderGraph::ResourceState state);

    void Compile();
    void Execute(const void* user_data, vk::CommandBuffer command_buffer,
                 std::uint32_t image_index, std::uint32_t fif_slot);

    [[nodiscard]] bool IsCompiled() const { return compiled_; }
    [[nodiscard]] const VulkanEngine::RenderGraph::CompiledRenderGraph& GetCompiledGraph() const { return compiled_graph_; }

    // Engine-owned pipeline declaration for a pass (nullptr when none declared).
    [[nodiscard]] const VulkanEngine::PipelinePass::PassPipelineRequest* GetPassPipelineRequest(
        VulkanEngine::RenderGraph::PassHandle handle) const;
    [[nodiscard]] vk::PipelineLayout GetPassPipelineLayout(VulkanEngine::RenderGraph::PassHandle handle) const;
    [[nodiscard]] vk::PipelineLayout GetPassPipelineLayoutByName(std::string_view name) const;

private:
    void SyncTransients();
    void BuildPassPipelines();
    void PollPassPipelines(std::uint32_t fif_slot);
    void ResolveResources(VulkanEngine::RenderGraph::CompiledRenderGraph& graph,
                          std::uint32_t image_index, std::uint32_t fif_slot);

    VulkanBackend::Vulkan::VulkanBootstrap* bootstrap_ = nullptr;
    ShaderSystem::ShaderManager* shader_manager_ = nullptr;
    ShaderSystem::PipelineFactory* pipeline_factory_ = nullptr;
    std::array<vk::DescriptorSetLayout, 5> engine_set_layouts_{};
    VulkanEngine::RenderGraph::RenderGraphBuilder graph_builder_{};
    VulkanEngine::RenderGraph::CompiledRenderGraph compiled_graph_{};

    std::unordered_map<std::uint32_t, TransientImageDesc> transient_image_descs_{};
    std::unordered_map<std::uint32_t, TransientBufferDesc> transient_buffer_descs_{};
    VulkanEngine::GpuResources::TransientAllocator transient_allocator_{};

    struct ExternalResourceResolver {
        ImageResolver resolve_image;
        ImageViewResolver resolve_image_view;
        vk::Format format = vk::Format::eUndefined;
    };
    std::unordered_map<std::string, ExternalResourceResolver> resource_resolvers_{};
    std::unordered_map<std::string, BufferResolver> buffer_resolvers_{};
    std::unordered_set<std::string> image_resolver_names_{};
    std::unordered_set<std::string> buffer_resolver_names_{};

    VulkanEngine::RenderGraph::ResourceHandle backbuffer_handle_{};
    VulkanEngine::RenderGraph::ResourceHandle depth_buffer_handle_{};

    std::uint32_t backbuffer_resource_index_ = 0;
    std::uint32_t depth_buffer_resource_index_ = 0;

    // Per swapchain image (outer index) and per resource index (inner), the
    // actual end-of-frame layout recorded last time that image was used. Seeded
    // back into the barrier plan so imported layouts are not reset to Undefined.
    std::vector<std::vector<VulkanEngine::RenderGraph::ResourceState>> tracked_states_{};
    std::vector<std::vector<bool>> tracked_valid_{};
    std::uint32_t tracked_resource_count_ = 0;

    // Custom pass storage and built-in handles
    std::vector<std::unique_ptr<VulkanEngine::PipelinePass::IPipelinePass>> custom_passes_{};
    std::array<VulkanEngine::RenderGraph::PassHandle, 6> builtin_handles_{};

    // Engine-owned pass pipelines: one retire ring per pass that declared a
    // pipeline request. The engine builds the VkPipeline from the request (and
    // hot-reloads it); the pass never owns a pipeline.
    struct PassPipelineState {
        std::string name{};
        VulkanEngine::PipelinePass::PassPipelineRequest request{};
        std::vector<VulkanEngine::Render::DescriptorDecl> bindings{};
        ShaderSystem::PipelineSlot slot{};
        bool built = false;
        // Descriptor set layouts must outlive the pipeline layout that references
        // them, so the state owns both.
        std::vector<vk::raii::DescriptorSetLayout> app_set_layouts{};
        std::unique_ptr<vk::raii::PipelineLayout> layout{};
        // Stable storage for pointer-bearing desc fields (hot reload reuses it).
        vk::PipelineColorBlendAttachmentState color_blend_attachment{};
        ShaderSystem::GraphicsPipelineDesc graphics_desc{};
        ShaderSystem::ComputePipelineDesc compute_desc{};
    };
    std::unordered_map<std::uint32_t, PassPipelineState> pass_pipelines_{};
    std::unordered_map<std::string, std::uint32_t> pass_pipeline_by_name_{};

    // Per-frame name -> resolved-handle table handed to passes via FrameContext.
    VulkanEngine::PipelinePass::ResourceLookupTable frame_lookup_{};

    bool compiled_ = false;
    bool initialized_ = false;
};

} // namespace VulkanEngine::RenderPipeline
