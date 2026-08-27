module;

export module VulkanEngine.PipelineFactory;

import std;
import vulkan_hpp;
import ShaderReflection;
import VulkanEngine.ShaderManager;
import VulkanEngine.GplPolicy;
import VulkanBackend.Vulkan.VulkanCapabilities;

export namespace VulkanEngine::ShaderSystem {

struct GraphicsPipelineDesc {
    ShaderId vertex_shader;
    ShaderId fragment_shader;

    vk::PipelineVertexInputStateCreateInfo vertex_input;
    vk::PipelineInputAssemblyStateCreateInfo input_assembly;
    vk::PipelineViewportStateCreateInfo viewport;
    vk::PipelineRasterizationStateCreateInfo rasterization;
    vk::PipelineMultisampleStateCreateInfo multisample;
    vk::PipelineDepthStencilStateCreateInfo depth_stencil;
    vk::PipelineColorBlendStateCreateInfo color_blend;
    std::vector<vk::DynamicState> dynamic_states;

    vk::PipelineLayout layout;

    std::vector<vk::Format> color_formats;
    vk::Format depth_format = vk::Format::eUndefined;
    vk::Format stencil_format = vk::Format::eUndefined;
};

struct ComputePipelineDesc {
    ShaderId shader;
    vk::PipelineLayout layout;
};

class PipelineProduct {
public:
    PipelineProduct() : linked_(nullptr) {}

    static PipelineProduct Monolithic(vk::raii::Pipeline pipeline);

    static PipelineProduct GPLLinked(
        vk::raii::Pipeline linked,
        std::shared_ptr<vk::raii::Pipeline> vertex_input_lib,
        std::shared_ptr<vk::raii::Pipeline> pre_raster_lib,
        std::shared_ptr<vk::raii::Pipeline> fragment_shader_lib,
        std::shared_ptr<vk::raii::Pipeline> fragment_output_lib);

    ~PipelineProduct() = default;

    PipelineProduct(PipelineProduct&&) = default;
    PipelineProduct& operator=(PipelineProduct&&) = default;

    [[nodiscard]] vk::Pipeline Get() const { return *linked_; }

    PipelineProduct(const PipelineProduct&) = delete;
    PipelineProduct& operator=(const PipelineProduct&) = delete;

private:
    vk::raii::Pipeline linked_;
    // Fast-linked pipelines (no VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT)
    // reference the library pipelines they were linked from. Each product keeps
    // its libraries alive — including while retired — so the shared cache can
    // drop entries (e.g. InvalidateShader) without dangling the linked pipeline.
    std::shared_ptr<vk::raii::Pipeline> vertex_input_lib_;
    std::shared_ptr<vk::raii::Pipeline> pre_raster_lib_;
    std::shared_ptr<vk::raii::Pipeline> fragment_lib_;
    std::shared_ptr<vk::raii::Pipeline> fragment_output_lib_;
};

class PipelineSlot {
public:
    static constexpr std::uint32_t kMaxFramesInFlight = 3;

    PipelineSlot() = default;

    void Swap(PipelineProduct product, std::uint32_t frame_index);

    void RetireFrame(std::uint32_t frame_index);

    [[nodiscard]] vk::Pipeline Get() const;

    bool PollAndRebuild(ShaderManager& shaders, ShaderId id,
                        std::function<std::optional<PipelineProduct>(ShaderManager&)> rebuild_fn,
                        std::uint32_t frame_index);

    bool PollAndRebuild(ShaderManager& shaders, ShaderId vert_id, ShaderId frag_id,
                        std::function<std::optional<PipelineProduct>(ShaderManager&)> rebuild_fn,
                        std::uint32_t frame_index);

private:
    PipelineProduct current_;
    std::array<std::vector<PipelineProduct>, kMaxFramesInFlight> retiring_;
    std::uint64_t last_vert_version_{0};
    std::uint64_t last_frag_version_{0};
};

class PipelineFactory {
public:
    PipelineFactory(const vk::raii::Device& device, const VulkanBackend::Vulkan::VulkanCapabilities& caps,
                    const vk::raii::PipelineCache& cache, GplPolicy policy = GplPolicy::Auto,
                    GplStructurePolicy structure = GplStructurePolicy::Auto);

    [[nodiscard]] std::expected<PipelineProduct, vk::Result>
        CreateGraphics(const GraphicsPipelineDesc& desc,
                       ShaderManager& shaders) const;

    [[nodiscard]] std::expected<PipelineProduct, vk::Result>
        CreateCompute(const ComputePipelineDesc& desc,
                      ShaderManager& shaders) const;

    [[nodiscard]] bool IsGPLAvailable() const { return gpl_available_; }

    void InvalidateShader(ShaderId id);

private:
    [[nodiscard]] PipelineProduct
        CreateGraphicsMonolithic(const GraphicsPipelineDesc& desc,
                                 ShaderManager& shaders) const;

    [[nodiscard]] PipelineProduct
        CreateGraphicsGPL(const GraphicsPipelineDesc& desc,
                          ShaderManager& shaders) const;

    // Final fast-link (no LTO): returns nullopt on VK_PIPELINE_COMPILE_REQUIRED
    // ("cannot fast-link") — the caller falls back to monolithic.
    [[nodiscard]] std::optional<vk::raii::Pipeline>
        CreateGraphicsGPLFinalLink(const GraphicsPipelineDesc& desc,
                                   std::span<const vk::Pipeline> libraries) const;

    static std::uint64_t HashVertexInput(const vk::PipelineVertexInputStateCreateInfo& vi,
                                         const vk::PipelineInputAssemblyStateCreateInfo& ia);
    static std::uint64_t HashPreRaster(
        const vk::PipelineViewportStateCreateInfo& vp,
        const vk::PipelineRasterizationStateCreateInfo& rs,
        const vk::PipelineMultisampleStateCreateInfo& ms,
        const std::vector<vk::DynamicState>& dynamic_states,
        vk::PipelineLayout layout);
    static std::uint64_t HashFragmentShader(
        const vk::PipelineDepthStencilStateCreateInfo& ds,
        vk::PipelineLayout layout);
    static std::uint64_t HashFragmentOutput(
        const vk::PipelineMultisampleStateCreateInfo& ms,
        const vk::PipelineColorBlendStateCreateInfo& cb,
        std::span<const vk::Format> color_formats,
        vk::Format depth_fmt,
        vk::Format stencil_fmt);

    const vk::raii::Device& device_;
    const vk::raii::PipelineCache& cache_;
    bool gpl_available_;
    GplResolution resolution_;

    struct SharedLibraries {
        struct LibraryEntry {
            ShaderId shader_id{};
            std::uint64_t shader_version{0};
            std::shared_ptr<vk::raii::Pipeline> pipeline;
        };
        std::unordered_map<std::uint64_t, LibraryEntry> vertex_input;
        std::unordered_map<std::uint64_t, LibraryEntry> pre_raster;
        std::unordered_map<std::uint64_t, LibraryEntry> fragment_shader;
        // Shader-independent (no shader_id) — never erased by InvalidateShader.
        std::unordered_map<std::uint64_t, LibraryEntry> fragment_output;
        mutable std::shared_mutex mutex;
    };
    std::shared_ptr<SharedLibraries> shared_;
};

} // namespace VulkanEngine::ShaderSystem
