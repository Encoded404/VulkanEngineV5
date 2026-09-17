module;
#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)

export module VulkanEngine.SceneRenderer;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanEngine.ECS.ComponentRegistry;
export import VulkanBackend.Vulkan.VulkanBootstrap;
export import VulkanShared.CallbackList;
export import VulkanEngine.Components.Transform;
export import VulkanEngine.Components.MeshReference;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.TechniqueManager;
export import VulkanEngine.BindlessManager;
export import VulkanEngine.Mesh.MeshTypes;
export import VulkanEngine.GpuResources;
export import VulkanEngine.GpuResources.BlockArray;
import VulkanEngine.PipelineFactory;
import VulkanEngine.ShaderManager;
import VulkanEngine.ShaderRegistration;

export namespace VulkanEngine::SceneRenderer {

// NOLINTBEGIN(modernize-avoid-c-arrays)
// ── Scene uniform data (replaces push-constant lighting) ──
struct alignas(16) Light {
    float position[4];
    float color[4];
    float direction[4];
    float params[4];
};

struct alignas(16) SceneHeader {
    float ambient_color[4];
    float sun_direction[4];
    float sun_color[4];
    std::uint32_t light_count = 0;
};
// NOLINTEND(modernize-avoid-c-arrays)

constexpr std::uint32_t LIGHTS_PER_BLOCK = 256;
constexpr std::uint32_t MAX_LIGHT_BLOCKS = 16;

// ── GPU technique flag-table bit layout ──
// SceneRenderer::UpdateTechniqueFlags packs BaseTechnique::PipelineFlags into a
// uint[MAX_TECHNIQUES] storage buffer consumed by the occluder-select,
// pre-cull and occlusion cull shaders. Bit positions must match the kFlag*
// constants in occluder_select.slang, pre_cull.slang and occlusion_cull.slang.
inline constexpr std::uint32_t TECHNIQUE_FLAG_DEPTH_PASS = 1u << 0;           // participates_in_depth_pass
inline constexpr std::uint32_t TECHNIQUE_FLAG_RECEIVES_OCCLUSION = 1u << 1;   // receives_occlusion
inline constexpr std::uint32_t TECHNIQUE_FLAG_COLLECT = 1u << 2;              // participates_in_collect
inline constexpr std::uint32_t TECHNIQUE_FLAG_OCCLUDER_SAFE = 1u << 3;        // bounds_conservative (§5.5)

// Occluder selection: coverage threshold as a fraction of screen area and the
// entry budget (docs/pre-prepass-occlusion-culling.md §5.1-A).
inline constexpr float kOccluderMinAreaFraction = 0.0025f; // 0.25 % of the screen

// ── Indexed-drawing pipeline (docs/indexed-drawing-pipeline.md) ──
// Compaction output / draw shape. Fixed at initialization; changing it
// re-creates the mode-dependent frame buffers and rebuilds the
// kCompactionMode-specialized compute pipelines.
enum class DrawMode : std::uint8_t {
    Monolithic = 0,      // 4 B absolute slot indices, one drawIndexedIndirect per pass/technique
    MultiIndirect = 1,   // 20 B DrawIndexedIndirectCommand per alive submesh, drawIndexedIndirectCount
};

// Runtime scene totals that size the per-frame indexed-drawing buffers. Since
// MeshRenderSystem::ProcessFrame accumulates the real values after upload,
// EnsureSceneCapacity grows the ring geometrically rather than relying on the
// initial (init-time) estimate.
struct SceneCapacity {
    std::uint32_t index_count = 0;   // Σ submesh index_count over the frame
    std::uint32_t vertex_span = 0;   // Σ submesh tight vertex-window span
    std::uint32_t submesh_count = 0; // Σ submeshes (alive + culled) over the frame
};

class SceneRenderer {
public:
    static constexpr std::uint32_t MAX_HIZ_MIPS = 12;
    // DispatchHiZGen emits up to this many levels per iteration (base and
    // base+1). The generator and the initialization-time coverage assert share
    // this value so the two cannot drift apart.
    static constexpr std::uint32_t HIZ_BATCH = 2;
    static constexpr std::uint32_t MAX_VERTEX_BUFFERS = 64;
    static constexpr std::uint32_t MAX_INDEX_BUFFERS = 64;
    static constexpr std::uint32_t BLOCK_ENTRIES = 256;
    static constexpr std::uint32_t MAX_BLOCKS = 1024;
    static constexpr std::uint32_t MAX_TECHNIQUES = 256;

    // Highest Hi-Z level covered by whole HIZ_BATCH-sized DispatchHiZGen
    // iterations for a `mip_count`-level pyramid. The cull shaders clamp their
    // sampling independently (MaxHizMip() = floor(log2(max(hizW, hizH))));
    // Initialize asserts the two derivations agree so a sampled level can never
    // be left unwritten.
    static constexpr std::uint32_t LastHiZLevelWritten(std::uint32_t mip_count) {
        std::uint32_t last = 0;
        for (std::uint32_t bl = 0; bl < mip_count; bl += HIZ_BATCH) {
            last = bl + ((mip_count - bl < HIZ_BATCH) ? (mip_count - bl) : HIZ_BATCH) - 1;
        }
        return last;
    }

    SceneRenderer() = default;
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    bool Initialize(VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                    VulkanEngine::GpuResources::DeviceBufferHeap& vertex_heap,
                    SceneCapacity initial_capacity,
                    ShaderSystem::ShaderManager& shader_mgr,
                    ShaderSystem::PipelineFactory& pipeline_factory,
                    const EngineShaderIds& shader_ids,
                    std::uint32_t frames_in_flight,
                    DrawMode draw_mode = DrawMode::Monolithic);
    void Shutdown();

    // Grows (never shrinks) the indexed-drawing frame ring to cover the given
    // scene totals. Called from the gather pass once the frame's real totals
    // are known. Device-idles and re-creates the capacity-dependent buffers
    // when growth is required; no-op otherwise.
    void EnsureSceneCapacity(const SceneCapacity& required);

    // Switches draw mode at runtime: device idle, destroy + re-create the
    // mode-dependent buffers at the current capacity, then rebuild the
    // kCompactionMode-specialized compute pipelines. Descriptor set layouts,
    // pools and technique graphics pipelines are mode-independent and reused.
    void Reinitialize(DrawMode mode);

    // Re-creates the resolution-dependent Hi-Z image ring and sampler when the
    // swapchain render extent changes (window resize). Idempotent: no-op when
    // the extent already matches the current allocation. Called once per frame
    // from the renderer after the swapchain extent is known. The render graph
    // needs no rebuild: the "hiz-image" resolver re-reads the handles each
    // frame.
    void EnsureRenderExtent(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] DrawMode GetDrawMode() const { return draw_mode_; }
    [[nodiscard]] const SceneCapacity& GetSceneCapacity() const { return scene_capacity_; }
    [[nodiscard]] bool IsDrawModeSupported(DrawMode mode) const;
    [[nodiscard]] std::uint32_t GetTechniqueRegionTotal() const { return region_total_; }

    // MID main pass: CPU prefix-sum of submesh counts per technique. Defines
    // each technique's fixed command region in the shared command buffer.
    void SetTechniqueCommandRegions(std::span<const std::uint32_t> submeshes_per_technique);

    [[nodiscard]] vk::DescriptorSetLayout* GetSubmeshVertexDataLayout() const;
    [[nodiscard]] vk::DescriptorSetLayout* GetRawVertexLayout() const;
    [[nodiscard]] vk::DescriptorSetLayout* GetIndirectionLayout() const;

    void UpdateVertexBufferArrayElement(std::uint32_t frame_index, std::uint32_t buffer_index, vk::Buffer buffer, std::uint64_t size);
    void UpdateIndexBufferArrayElement(std::uint32_t frame_index, std::uint32_t buffer_index, vk::Buffer buffer, std::uint64_t size);
    void UpdateAllFrameVertexBufferArrayElements(std::uint32_t buffer_index, vk::Buffer buffer, std::uint64_t size);
    void UpdateAllFrameIndexBufferArrayElements(std::uint32_t buffer_index, vk::Buffer buffer, std::uint64_t size);

    void PrepareCompute(vk::CommandBuffer cmd,
                        VulkanEngine::ComponentRegistry& registry,
                        const glm::mat4& view_matrix,
                        const glm::mat4& projection_matrix,
                        std::uint32_t width,
                        std::uint32_t height,
                        std::uint32_t frame_index);

    void PollShaders(std::uint32_t frame_counter);

    void DepthPrepass(vk::CommandBuffer cmd,
                      std::uint32_t width,
                      std::uint32_t height,
                      std::uint32_t frame_index);

    void Render(vk::CommandBuffer cmd,
                VulkanEngine::ComponentRegistry& registry,
                VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr,
                VulkanEngine::BindlessManager::BindlessManager& bindless_mgr,
                const glm::mat4& projection_matrix,
                const glm::mat4& view_matrix,
                std::uint32_t width,
                std::uint32_t height,
                std::uint32_t frame_index);

    void DispatchHiZGen(vk::CommandBuffer cmd,
                        std::uint32_t width,
                        std::uint32_t height,
                        std::uint32_t frame_index,
                        std::uint32_t image_index);

    void DispatchOcclusion(vk::CommandBuffer cmd,
                           std::uint32_t frame_index);

    void DispatchCollect(vk::CommandBuffer cmd, std::uint32_t frame_index);

    void DispatchOccluderSelect(vk::CommandBuffer cmd,
                                 std::uint32_t width,
                                 std::uint32_t height,
                                 std::uint32_t frame_index);

    // Hardware raster of the selected occluder meshes into the depth prepass
    // buffer (load_op = CLEAR, store = STORE; render graph node).
    void OccluderPrepass(vk::CommandBuffer cmd,
                         std::uint32_t width,
                         std::uint32_t height,
                         std::uint32_t frame_index);

    void DispatchPreCull(vk::CommandBuffer cmd, std::uint32_t frame_index);

    // Packs each registered technique's PipelineFlags into the shared flag
    // table buffer (upload on change only). Called once per frame before
    // PrepareCompute.
    void UpdateTechniqueFlags(
        VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr);

    void InitializeHizFirstFrame(vk::CommandBuffer cmd);

    void SetSubmeshes(const std::vector<VulkanEngine::SubMesh>& submeshes) { scene_submeshes_ = submeshes; }
    [[nodiscard]] const std::vector<VulkanEngine::SubMesh>& GetSubmeshes() const { return scene_submeshes_; }

    struct FrameBlockArrays {
        VulkanEngine::GpuResources::BlockArray* compact_dynamic;
        VulkanEngine::GpuResources::BlockArray* compact_static;
        VulkanEngine::GpuResources::BlockArray* bounding_spheres;
        VulkanEngine::GpuResources::BlockArray* bounding_obb;
    };
    [[nodiscard]] FrameBlockArrays GetFrameBlockArrays(std::uint32_t frame_index);

    [[nodiscard]] std::uint32_t GetCurrentEntityCount() const { return current_entity_count_; }
    void SetCurrentEntityCount(std::uint32_t count) { current_entity_count_ = count; }

    [[nodiscard]] const glm::mat4& GetViewProj() const { return view_proj_; }
    void SetViewProj(const glm::mat4& vp) { view_proj_ = vp; }

    [[nodiscard]] vk::Image GetHizImage(std::uint32_t frame_index) const {
        const auto& frame = frames_[frame_index % frames_in_flight_];
        return static_cast<vk::Image>(*frame.hiz_image);
    }
    [[nodiscard]] vk::ImageView GetHizFullView(std::uint32_t frame_index) const {
        const auto& frame = frames_[frame_index % frames_in_flight_];
        return static_cast<vk::ImageView>(*frame.hiz_full_view);
    }
    void UpdateHizDepthBinding(std::uint32_t frame_index, vk::ImageView depth_view);

    void DispatchExpand(vk::CommandBuffer cmd, std::uint32_t object_count,
                        const glm::mat4& view_proj, std::uint32_t frame_index);

    // Buffer access for render graph
    [[nodiscard]] vk::Buffer GetTechniqueDrawCommandsBuffer(std::uint32_t frame_index) const {
        const auto& fr = frames_[frame_index % frames_in_flight_];
        return static_cast<vk::Buffer>(*fr.technique_draw_commands.GetBuffer());
    }

    // ── Lighting system (descriptor set 4) — binding 0 = SceneHeader, binding 1 = Light[] BlockArray ──
    [[nodiscard]] vk::DescriptorSetLayout GetSceneUniformLayout() const {
        return *scene_uniform_layout_;
    }
    [[nodiscard]] vk::DescriptorSet GetSceneUniformSet() const {
        return *scene_uniform_set_;
    }
    void UploadLighting(const SceneHeader& header,
                        std::span<const Light> lights,
                        VulkanEngine::GpuResources::StagingManager& staging);

private:
    struct TechniqueResult { std::uint32_t offset; std::uint32_t count; };
    struct FrameResources {
        // Block-based per-submesh buffers
        VulkanEngine::GpuResources::BlockArray compact_dynamic{};
        VulkanEngine::GpuResources::BlockArray compact_static{};
        VulkanEngine::GpuResources::BlockArray bounding_spheres{};
        VulkanEngine::GpuResources::BlockArray bounding_obb{};
        VulkanEngine::GpuResources::BlockArray submesh_vertex_data{};
        VulkanEngine::GpuResources::BlockArray submesh_cull{};

        // ── Indexed-drawing substrate (shared by both modes) ──
        // One IndirEntry per slot in each submesh's tight vertex window.
        VulkanEngine::GpuResources::GpuBuffer vertex_entries{};
        // 4 B absolute slot indices into vertex_entries, one per occurrence.
        VulkanEngine::GpuResources::GpuBuffer draw_indices{};
        // 2 x u32 atomic counters allocated by expand: [0]=entryBase, [1]=indexBase.
        VulkanEngine::GpuResources::GpuBuffer expand_counter{};

        // ── Monolithic destinations (4 B compact index lists) ──
        VulkanEngine::GpuResources::GpuBuffer main_compact_indices{};
        VulkanEngine::GpuResources::GpuBuffer depth_compact_indices{};
        VulkanEngine::GpuResources::GpuBuffer occluder_compact_indices{};
        // Monolithic GPU-accumulated indexed draw commands (word 0 = indexCount).
        VulkanEngine::GpuResources::GpuBuffer depth_draw_command{};
        VulkanEngine::GpuResources::GpuBuffer occluder_draw_command{};

        // ── MID destinations (20 B indexed indirect commands + count) ──
        VulkanEngine::GpuResources::GpuBuffer main_commands{};
        VulkanEngine::GpuResources::GpuBuffer depth_commands{};
        VulkanEngine::GpuResources::GpuBuffer occluder_commands{};
        VulkanEngine::GpuResources::GpuBuffer depth_command_count{};
        VulkanEngine::GpuResources::GpuBuffer occluder_command_count{};
        // CPU prefix sum of submeshes per technique (host-visible, MID main pass).
        VulkanEngine::GpuResources::GpuBuffer region_base_buffer{};

        VulkanEngine::GpuResources::GpuBuffer occluder_count_buffer{};
        VulkanEngine::GpuResources::GpuBuffer technique_draw_commands{};
        VulkanEngine::GpuResources::GpuBuffer tech_counts_buffer{};
        VulkanEngine::GpuResources::GpuBuffer intermediate_buffer{};

        // Descriptor sets
        VulkanEngine::GpuResources::GpuDescriptorSet expand_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet occlusion_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet collect_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet collect_write_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet occluder_select_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet submesh_vertex_set{};
        // Single shared indirection set (set 3) bound to vertex_entries and
        // reused by the depth, occluder and main passes.
        vk::raii::DescriptorSet indirection_raw_set = vk::raii::DescriptorSet(nullptr);
        vk::raii::DescriptorSet bindless_vertex_set = vk::raii::DescriptorSet(nullptr);
        vk::raii::DescriptorSet bindless_index_set = vk::raii::DescriptorSet(nullptr);
        VulkanEngine::GpuResources::GpuDescriptorSet hiz_set{};

        vk::raii::Image hiz_image = vk::raii::Image(nullptr);
        vk::raii::DeviceMemory hiz_memory = vk::raii::DeviceMemory(nullptr);
        std::vector<vk::raii::ImageView> hiz_mip_views{};
        vk::raii::ImageView hiz_full_view = vk::raii::ImageView(nullptr);

        // Projection coefficients for the occlusion culler (see OccPC.projInfo).
        glm::vec4 proj_info{};
    };

    bool CreateExpandPipeline(const VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                              ShaderSystem::ShaderManager& shader_mgr,
                              ShaderSystem::PipelineFactory& pipeline_factory,
                              ShaderSystem::ShaderId shader_id);
    bool CreateDepthPipeline(VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                             ShaderSystem::ShaderManager& shader_mgr,
                             ShaderSystem::PipelineFactory& pipeline_factory,
                             ShaderSystem::ShaderId vert_id,
                             ShaderSystem::ShaderId frag_id,
                             const vk::PipelineRasterizationStateCreateInfo& rasterization);
    bool CreateHiZPipeline(VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                           ShaderSystem::ShaderManager& shader_mgr,
                           ShaderSystem::PipelineFactory& pipeline_factory,
                           ShaderSystem::ShaderId shader_id);
    bool CreateOcclusionPipeline(const VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                                  ShaderSystem::ShaderManager& shader_mgr,
                                  ShaderSystem::PipelineFactory& pipeline_factory,
                                  ShaderSystem::ShaderId shader_id);
    bool CreateOccluderSelectPipeline(const VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                                       ShaderSystem::ShaderManager& shader_mgr,
                                       ShaderSystem::PipelineFactory& pipeline_factory,
                                       ShaderSystem::ShaderId shader_id);
    bool CreatePreCullPipeline(const VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                                ShaderSystem::ShaderManager& shader_mgr,
                                ShaderSystem::PipelineFactory& pipeline_factory,
                                ShaderSystem::ShaderId shader_id);
    bool CreateCollectPipelines(const VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                                 ShaderSystem::ShaderManager& shader_mgr,
                                 ShaderSystem::PipelineFactory& pipeline_factory,
                                 ShaderSystem::ShaderId count_id,
                                 ShaderSystem::ShaderId write_id);


    void UpdateBlockArrayDescriptor(vk::DescriptorSet desc_set, std::uint32_t binding,
                                      VulkanEngine::GpuResources::BlockArray& block_buf,
                                      vk::DescriptorType desc_type);

    VulkanBackend::Vulkan::IVulkanBootstrap* backend_ = nullptr;

    // Hot-reload rebuild support: kept refs + per-pipeline creation descs so the
    // frame loop can re-create pipelines when a shader's version changes.
    ShaderSystem::ShaderManager* shader_mgr_ = nullptr;
    ShaderSystem::PipelineFactory* pipeline_factory_ = nullptr;
    EngineShaderIds shader_ids_{};

    // Set 1: SubmeshVertexData blocks
    std::unique_ptr<vk::raii::DescriptorSetLayout> submesh_vertex_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> submesh_vertex_pool_;

    // Set 2: Bindless vertex buffer array
    std::unique_ptr<vk::raii::DescriptorSetLayout> raw_vertex_layout_{};
    std::unique_ptr<vk::raii::DescriptorPool> raw_vertex_pool_;

    // Set 3: Indirection buffer (depth uses dedicated set, main uses indirection_raw_set)
    std::unique_ptr<vk::raii::DescriptorSetLayout> indirection_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> indirection_pool_;
    std::unique_ptr<vk::raii::DescriptorPool> indirection_raw_pool_;

    // Set 4: Expand compute (block arrays + single buffers)
    std::unique_ptr<vk::raii::DescriptorSetLayout> expand_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> expand_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> expand_pipeline_layout_{};
    ShaderSystem::PipelineSlot expand_slot_;
    std::optional<ShaderSystem::ComputePipelineDesc> expand_desc_;


    // Set 5: Occlusion compute (blocks + Hi-Z)
    std::unique_ptr<vk::raii::DescriptorSetLayout> occlusion_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> occlusion_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> occlusion_pipeline_layout_{};
    ShaderSystem::PipelineSlot occlusion_slot_;
    std::optional<ShaderSystem::ComputePipelineDesc> occlusion_desc_;

    // Set 8: Occluder-select compute (cull blocks + transforms + OBBs + flag
    // table + indirection copy + draw command + selection counter)
    std::unique_ptr<vk::raii::DescriptorSetLayout> occluder_select_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> occluder_select_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> occluder_select_pipeline_layout_{};
    ShaderSystem::PipelineSlot occluder_select_slot_;
    std::optional<ShaderSystem::ComputePipelineDesc> occluder_select_desc_;

    // Pre-cull compute: reuses the occlusion set (extended with the survivor
    // compaction bindings 6-8) + its own pipeline.
    std::unique_ptr<vk::raii::PipelineLayout> pre_cull_pipeline_layout_{};
    ShaderSystem::PipelineSlot pre_cull_slot_;
    std::optional<ShaderSystem::ComputePipelineDesc> pre_cull_desc_;

    // Set 6: Collect count + compact (cull blocks + indirections + intermediate)
    std::unique_ptr<vk::raii::DescriptorSetLayout> collect_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> collect_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> collect_pipeline_layout_{};
    ShaderSystem::PipelineSlot collect_count_slot_;
    std::optional<ShaderSystem::ComputePipelineDesc> collect_count_desc_;
    // Collect write shaders (use separate layout)
    std::unique_ptr<vk::raii::DescriptorSetLayout> collect_write_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> collect_write_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> collect_write_pipeline_layout_{};
    ShaderSystem::PipelineSlot collect_write_slot_;
    std::optional<ShaderSystem::ComputePipelineDesc> collect_write_desc_;

    std::unique_ptr<vk::raii::Sampler> depth_sampler_{};
    std::unique_ptr<vk::raii::Sampler> hiz_sampler_{};

    std::uint32_t depth_width_ = 0;
    std::uint32_t depth_height_ = 0;
    std::uint32_t hiz_mip_count_ = 0;
    bool hiz_initialized_ = false;

    std::unique_ptr<vk::raii::DescriptorSetLayout> empty_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> empty_pool_{};
    std::vector<VulkanEngine::GpuResources::GpuDescriptorSet> empty_sets_{};

    std::unique_ptr<vk::raii::PipelineLayout> depth_pipeline_layout_{};
    ShaderSystem::PipelineSlot depth_slot_;
    std::optional<ShaderSystem::GraphicsPipelineDesc> depth_desc_;

    std::unique_ptr<vk::raii::DescriptorSetLayout> hiz_layout_{};
    std::unique_ptr<vk::raii::PipelineLayout> hiz_pipeline_layout_{};
    ShaderSystem::PipelineSlot hiz_slot_;
    std::optional<ShaderSystem::ComputePipelineDesc> hiz_desc_;
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> hiz_pool_;

    // Bindless index buffer array (used by expand at set 5)
    std::unique_ptr<vk::raii::DescriptorSetLayout> bindless_index_layout_{};
    std::unique_ptr<vk::raii::DescriptorPool> bindless_index_pool_;

    // Lighting system (set 4)
    VulkanEngine::GpuResources::GpuBuffer scene_header_buffer_{};
    VulkanEngine::GpuResources::BlockArray scene_light_blocks_{};
    std::unique_ptr<vk::raii::DescriptorSetLayout> scene_uniform_layout_{};
    std::unique_ptr<vk::raii::DescriptorPool> scene_uniform_pool_{};
    std::unique_ptr<vk::raii::DescriptorSet> scene_uniform_set_{};

    std::vector<VulkanEngine::SubMesh> scene_submeshes_{};

    // Shared (not per-frame) technique flag table: PipelineFlags change rarely,
    // so a single host-visible buffer is updated in place only when contents
    // change. Worst case on a change is one frame reading a torn word, which is
    // self-correcting next frame.
    VulkanEngine::GpuResources::GpuBuffer technique_flags_buffer_{};
    std::vector<std::uint32_t> technique_flags_cache_{};

    // Create/destroy the mode-dependent per-frame buffers at the current
    // capacity. Called by Initialize and EnsureSceneCapacity/Reinitialize.
    bool CreateFrameBuffers();
    void DestroyFrameBuffers();
    // Create/destroy the resolution-dependent Hi-Z image ring + sampler at the
    // current depth_width_/depth_height_. Called by Initialize and
    // EnsureRenderExtent. CreateHiZResources recomputes hiz_mip_count_, rewrites
    // the hiz-set storage/sampler/depth bindings, and asserts the generator's
    // mip coverage matches what the cull shaders sample.
    bool CreateHiZResources();
    void DestroyHiZResources();
    // Rebuild the kCompactionMode-specialized compute pipelines (pre-cull,
    // occluder-select, collect count/compact, collect write).
    bool RebuildCompactionPipelines();

    // Runtime-sized per-frame resource ring (see frames_in_flight_).
    std::vector<FrameResources> frames_;
    std::uint32_t frames_in_flight_ = 3;
    std::uint32_t current_entity_count_ = 0;
    glm::mat4 view_proj_{1.0f};

    DrawMode draw_mode_ = DrawMode::Monolithic;
    bool draw_indirect_count_supported_ = false;
    SceneCapacity scene_capacity_{};
    // CPU prefix sum (MID): region_base_[t] = first command slot of technique t.
    std::vector<std::uint32_t> region_base_{};
    std::vector<std::uint32_t> region_count_{};
    std::uint32_t region_total_ = 0;
};

}
