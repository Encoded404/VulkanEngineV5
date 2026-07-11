module;

export module VulkanEngine.RenderGraph;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanShared.RenderGraphTypes;

export namespace VulkanEngine::RenderGraph {

class RenderGraphBuilder {
public:
    struct ReadInfo {
        ResourceHandle resource{};
        PipelineStageIntent stage = PipelineStageIntent::FragmentShader;
        AccessIntent access = AccessIntent::Read;
    };

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
    bool AddRead(PassHandle pass, ResourceHandle resource,
                 PipelineStageIntent stage, AccessIntent access);
    bool AddWrite(PassHandle pass, ResourceHandle resource);
    bool AddDependency(PassHandle before, PassHandle after);

    bool SetPassAttachments(PassHandle pass, PassAttachmentSetup setup);

    [[nodiscard]] CompiledRenderGraph Compile() const;

private:
    struct ResourceNode {
        std::string name{};
        ResourceKind kind = ResourceKind::Image;
        std::uint32_t generation = 1;
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
        std::uint32_t generation = 1;
        bool enabled = true;
        std::vector<ReadInfo> reads{};
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

}  // namespace VulkanEngine::RenderGraph
