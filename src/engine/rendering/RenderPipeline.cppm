module;

export module VulkanEngine.RenderPipeline;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanBackend.RenderGraph;
export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.PipelinePass;

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
    std::function<void(const void* user_data, vk::CommandBuffer command_buffer)> execute{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

// TransientImageDesc is now provided by the VulkanEngine.PipelinePass module.
using VulkanEngine::PipelinePass::TransientImageDesc;

class RenderPipeline : public VulkanEngine::PipelinePass::IResourceRegistry {
public:
    RenderPipeline();
    ~RenderPipeline() override;

    void Initialize(VulkanEngine::Runtime::VulkanBootstrap& bootstrap);
    void Shutdown();

    // ── IResourceRegistry overrides ──
    VulkanEngine::RenderGraph::ResourceHandle ImportBackbuffer() override;
    VulkanEngine::RenderGraph::ResourceHandle ImportDepthBuffer() override;
    VulkanEngine::RenderGraph::ResourceHandle ImportImage(const std::string& name) override;
    VulkanEngine::RenderGraph::ResourceHandle ImportBuffer(const std::string& name) override;
    VulkanEngine::RenderGraph::ResourceHandle CreateTransientImage(const TransientImageDesc& desc) override;

    using ImageResolver = std::function<vk::Image(std::uint32_t image_index)>;
    using ImageViewResolver = std::function<vk::ImageView(std::uint32_t image_index)>;
    void RegisterResourceResolver(const std::string& name,
                                  ImageResolver resolve_image,
                                  ImageViewResolver resolve_image_view,
                                  vk::Format format);

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
    void Execute(const void* user_data, vk::CommandBuffer command_buffer, std::uint32_t image_index);

    [[nodiscard]] bool IsCompiled() const { return compiled_; }
    [[nodiscard]] const VulkanEngine::RenderGraph::CompiledRenderGraph& GetCompiledGraph() const { return compiled_graph_; }

private:
    void AllocateTransients();
    void DeallocateTransients();
    void ResolveResources(VulkanEngine::RenderGraph::CompiledRenderGraph& graph, std::uint32_t image_index);

    VulkanEngine::Runtime::VulkanBootstrap* bootstrap_ = nullptr;
    VulkanEngine::RenderGraph::RenderGraphBuilder graph_builder_{};
    VulkanEngine::RenderGraph::CompiledRenderGraph compiled_graph_{};

    std::unordered_map<std::uint32_t, TransientImageDesc> transient_image_descs_{};
    std::vector<vk::raii::Image> transient_images_{};
    std::vector<vk::raii::DeviceMemory> transient_memories_{};
    std::vector<vk::raii::ImageView> transient_image_views_{};

    struct ExternalResourceResolver {
        ImageResolver resolve_image;
        ImageViewResolver resolve_image_view;
        vk::Format format = vk::Format::eUndefined;
    };
    std::unordered_map<std::string, ExternalResourceResolver> resource_resolvers_{};

    VulkanEngine::RenderGraph::ResourceHandle backbuffer_handle_{};
    VulkanEngine::RenderGraph::ResourceHandle depth_buffer_handle_{};

    std::uint32_t backbuffer_resource_index_ = 0;
    std::uint32_t depth_buffer_resource_index_ = 0;
    std::vector<bool> swapchain_image_presented_{};
    std::vector<bool> swapchain_depth_initialized_{};

    // Custom pass storage and built-in handles
    std::vector<std::unique_ptr<VulkanEngine::PipelinePass::IPipelinePass>> custom_passes_{};
    std::array<VulkanEngine::RenderGraph::PassHandle, 6> builtin_handles_{};

    bool compiled_ = false;
    bool initialized_ = false;
};

} // namespace VulkanEngine::RenderPipeline
