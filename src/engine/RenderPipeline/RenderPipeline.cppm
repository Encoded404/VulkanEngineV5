module;

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.RenderPipeline;

export import VulkanBackend.RenderGraph;
export import VulkanBackend.Runtime.VulkanBootstrap;

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

struct TransientImageDesc {
    std::string name{}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::Format format = vk::Format::eUndefined; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t width = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t height = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::ImageLayout initial_layout = vk::ImageLayout::eUndefined; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::ImageLayout final_layout = vk::ImageLayout::eUndefined; // NOLINT(misc-non-private-member-variables-in-classes)
};

class RenderPipeline {
public:
    RenderPipeline();
    ~RenderPipeline();

    void Initialize(VulkanEngine::Runtime::VulkanBootstrap& bootstrap);
    void Shutdown();

    VulkanEngine::RenderGraph::ResourceHandle ImportBackbuffer();
    VulkanEngine::RenderGraph::ResourceHandle ImportDepthBuffer();
    VulkanEngine::RenderGraph::ResourceHandle ImportImage(const std::string& name);
    VulkanEngine::RenderGraph::ResourceHandle ImportBuffer(const std::string& name);
    VulkanEngine::RenderGraph::ResourceHandle CreateTransientImage(const TransientImageDesc& desc);

    using ImageResolver = std::function<vk::Image(uint32_t image_index)>;
    using ImageViewResolver = std::function<vk::ImageView(uint32_t image_index)>;
    void RegisterResourceResolver(const std::string& name,
                                  ImageResolver resolve_image,
                                  ImageViewResolver resolve_image_view,
                                  vk::Format format);

    VulkanEngine::RenderGraph::PassHandle AddPass(const RenderPipelinePassDesc& desc);

    bool AddDependency(VulkanEngine::RenderGraph::PassHandle before,
                       VulkanEngine::RenderGraph::PassHandle after);

    bool SetInitialState(VulkanEngine::RenderGraph::ResourceHandle resource, VulkanEngine::RenderGraph::ResourceState state);
    bool SetFinalState(VulkanEngine::RenderGraph::ResourceHandle resource, VulkanEngine::RenderGraph::ResourceState state);

    void Compile();
    void Execute(const void* user_data, vk::CommandBuffer command_buffer, uint32_t image_index);

    [[nodiscard]] bool IsCompiled() const { return compiled_; }
    [[nodiscard]] const VulkanEngine::RenderGraph::CompiledRenderGraph& GetCompiledGraph() const { return compiled_graph_; }

private:
    void AllocateTransients();
    void DeallocateTransients();
    void ResolveResources(VulkanEngine::RenderGraph::CompiledRenderGraph& graph, uint32_t image_index);

    VulkanEngine::Runtime::VulkanBootstrap* bootstrap_ = nullptr;
    VulkanEngine::RenderGraph::RenderGraphBuilder graph_builder_{};
    VulkanEngine::RenderGraph::CompiledRenderGraph compiled_graph_{};

    std::unordered_map<uint32_t, TransientImageDesc> transient_image_descs_{};
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

    uint32_t backbuffer_resource_index_ = 0;
    uint32_t depth_buffer_resource_index_ = 0;
    std::vector<bool> swapchain_image_presented_{};
    std::vector<bool> swapchain_depth_initialized_{};

    bool compiled_ = false;
    bool initialized_ = false;
};

} // namespace VulkanEngine::RenderPipeline
