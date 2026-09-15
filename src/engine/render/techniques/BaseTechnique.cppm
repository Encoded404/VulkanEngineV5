module;

#include <cassert>

export module VulkanEngine.TechniqueManager.BaseTechnique;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanBackend.Vulkan.VulkanBootstrap;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.TechniqueManager.TechniqueId;
export import VulkanEngine.GpuResources.BlockArray;
export import VulkanEngine.GpuBuffer;
export import VulkanEngine.GpuResources.StagingManager;
import VulkanEngine.ShaderManager;
import VulkanEngine.PipelineFactory;

export namespace VulkanEngine::TechniqueManager {

// ── Configurable bit packing for StaticEntry.technique_material ──
// The 32-bit field is split into a technique id (low bits) and a material id
// (high bits). Change TECHNIQUE_BITS to redistribute the budget; keep
// kTechniqueBits in expand.slang in sync.
namespace TechniquePacking {
    inline constexpr std::uint32_t TECHNIQUE_BITS  = 14;                 // → 16384 techniques max
    inline constexpr std::uint32_t MATERIAL_BITS   = 32 - TECHNIQUE_BITS; // → 262144 materials max
    inline constexpr std::uint32_t TECHNIQUE_MASK  = (1u << TECHNIQUE_BITS) - 1;

    // Per-material BlockArray geometry. The fragment shaders hardcode this
    // split as materialId / 256 and materialId % 256, so it must remain 256.
    // MATERIAL_BLOCK_COUNT is the number of descriptor-array elements each
    // technique must expose for its per-material data, and must be able to
    // name every material id the packing above can produce.
    inline constexpr std::uint32_t MATERIAL_BLOCK_SIZE  = 256;
    inline constexpr std::uint32_t MATERIAL_BLOCK_COUNT =
        (1u << MATERIAL_BITS) / MATERIAL_BLOCK_SIZE;
    static_assert((1u << MATERIAL_BITS) % MATERIAL_BLOCK_SIZE == 0,
                  "MATERIAL_BITS must span a whole number of material blocks");

    constexpr std::uint32_t Pack(std::uint32_t material_id, std::uint32_t technique_id) {
        return (material_id << TECHNIQUE_BITS) | (technique_id & TECHNIQUE_MASK);
    }
    constexpr std::uint32_t UnpackTechnique(std::uint32_t packed) {
        return packed & TECHNIQUE_MASK;
    }
    constexpr std::uint32_t UnpackMaterial(std::uint32_t packed) {
        return packed >> TECHNIQUE_BITS;
    }
}

// ── PipelineFlags — per-technique pass participation hints ──
// SceneRenderer packs these into the GPU flag table (UpdateTechniqueFlags);
// the depth prepass filter, occluder selection and the occlusion cull shaders
// consume them.
struct PipelineFlags {
    bool participates_in_depth_pass = true;  // writes depth → occludes others
    bool receives_occlusion = true;          // gets culled by HiZ (set false for transparents)
    bool participates_in_collect = true;     // generates indirect draw commands
    // Conservative post-transform bounds (§5.5): techniques whose vertices can
    // be displaced by compute must set false — they are occludee-only and
    // never occluder candidates unless they prove a post-deform envelope.
    bool bounds_conservative = true;
};

// ── BaseTechnique — abstract base for all rendering techniques ──
class BaseTechnique {
public:
    enum class BindingKind : std::uint8_t {
        PerMaterial,  // bindless array indexed by material_id
        Shared,       // single buffer for all materials using this technique
    };

    struct BindingDecl {
        std::uint32_t set;
        std::uint32_t binding;
        BindingKind kind;
        std::uint32_t stride = 0;  // byte size per entry (PerMaterial only)
        std::type_index type_index = typeid(void);
    };

    virtual ~BaseTechnique() = default;

    [[nodiscard]] TechniqueId GetId() const { return id_; }
    [[nodiscard]] std::span<const BindingDecl> GetBindings() const { return bindings_; }
    [[nodiscard]] std::size_t GetBindingCount() const { return bindings_.size(); }
    [[nodiscard]] const BindingDecl& GetBinding(std::size_t i) const { return bindings_[i]; }

    // True when this technique declares a PerMaterial binding of exactly the
    // given data type. Callers must not reinterpret a material's packed data as
    // a type the technique did not declare (the layout would not match).
    [[nodiscard]] bool HasPerMaterialOfType(std::type_index type) const {
        return std::any_of(bindings_.begin(), bindings_.end(),
            [type](const BindingDecl& decl) {
                return decl.kind == BindingKind::PerMaterial && decl.type_index == type;
            });
    }

    // ── Typed access to shared data (technique-local, not per-material) ──
    template<typename T>
    const T& ReadShared() const {
        for (std::size_t i = 0; i < bindings_.size(); ++i) {
            if (bindings_[i].kind == BindingKind::Shared &&
                bindings_[i].type_index == std::type_index(typeid(T))) {
                assert(i < shared_cpu_data_.size());
                return *reinterpret_cast<const T*>(shared_cpu_data_[i].data());
            }
        }
        assert(!"Shared binding type not found");
        static T empty{};
        return empty;
    }

    // ── Update shared binding data (writes technique-local CPU buffer, stages upload to GPU) ──
    template<typename T>
    void UpdateShared(const T& data, VulkanEngine::GpuResources::StagingManager& staging) {
        for (std::size_t i = 0; i < bindings_.size(); ++i) {
            if (bindings_[i].kind == BindingKind::Shared &&
                bindings_[i].type_index == std::type_index(typeid(T))) {
                // Copy to technique-local CPU buffer
                assert(i < shared_cpu_data_.size());
                auto* dst = shared_cpu_data_[i].data();
                std::memcpy(dst, &data, sizeof(T));

                // Stage upload to GPU buffer
                assert(i < shared_buffers_.size());
                auto slice = staging.Allocate(sizeof(T), 256);
                std::memcpy(slice.data, &data, sizeof(T));
                staging.RecordBufferCopy(slice, *shared_buffers_[i].GetBuffer(), 0);
                return;
            }
        }
        assert(!"Shared binding type not found — did you declare it?");
    }

    // ── Block array access by type (PerMaterial only) ──
    template<typename T>
    VulkanEngine::GpuResources::BlockArray* GetBlockArrayForType() {
        std::size_t pm_ordinal = 0;
        for (std::size_t i = 0; i < bindings_.size(); ++i) {
            if (bindings_[i].kind != BindingKind::PerMaterial) continue;
            if (bindings_[i].type_index == std::type_index(typeid(T))) {
                assert(pm_ordinal < block_arrays_.size());
                return &block_arrays_[pm_ordinal];
            }
            ++pm_ordinal;
        }
        assert(!"PerMaterial binding type not found — did you declare it?");
        return nullptr;
    }

    // ── Block array access by declaration index into GetBindings() ──
    // Maps the binding's position among all bindings to its PerMaterial ordinal.
    [[nodiscard]] VulkanEngine::GpuResources::BlockArray* GetBlockArrayForBinding(
        std::size_t binding_index) {
        std::size_t pm_ordinal = 0;
        for (std::size_t i = 0; i < bindings_.size(); ++i) {
            if (bindings_[i].kind != BindingKind::PerMaterial) continue;
            if (i == binding_index) {
                return pm_ordinal < block_arrays_.size() ? &block_arrays_[pm_ordinal] : nullptr;
            }
            ++pm_ordinal;
        }
        return nullptr;
    }

    // ── Block array access by PerMaterial ordinal (0 = first PerMaterial binding) ──
    VulkanEngine::GpuResources::BlockArray* GetBlockArray(std::size_t block_array_index) {
        if (block_array_index < block_arrays_.size()) {
            return &block_arrays_[block_array_index];
        }
        return nullptr;
    }

    // ── GPU resource access ──
    [[nodiscard]] vk::Pipeline GetPipeline() const { return pipeline_slot_.Get(); }
    [[nodiscard]] vk::PipelineLayout GetPipelineLayout() const { return *pipeline_layout_; }

    // ── Custom descriptor sets (technique-owned BlockArray/Shared bindings at sets 4+) ──
    [[nodiscard]] std::span<const vk::DescriptorSet> GetCustomDescriptorSets() const {
        return { custom_descriptor_set_handles_.data(), custom_descriptor_set_handles_.size() };
    }

    // ── Material packing — each technique defines how to pack material_id into StaticEntry.technique_material ──
    // Default implementation uses TechniquePacking::Pack(material_id, technique_id).
    // Override for custom per-material data encoding (e.g., packing texture_slot for legacy shaders).
    [[nodiscard]] virtual uint32_t PackMaterialData(uint32_t material_id) const;

    // ── Per-material descriptor arrays ──
    // Bind block `block_index` of the per-material descriptor array for the
    // PerMaterial binding at `binding_index` in GetBindings() (same index the
    // BindingDecl/GpuResources::BlockArray helpers use). Idempotent, and safe
    // while frames are in flight: the binding is partially bound and
    // update-after-bind. Returns false when the index does not name a
    // PerMaterial binding or the BlockArray does not yet contain that block.
    [[nodiscard]] bool EnsureMaterialBlockBound(std::size_t binding_index, std::uint32_t block_index);

    // ── Pass participation flags — see PipelineFlags struct above ──
    PipelineFlags pipeline_flags{}; // NOLINT(misc-non-private-member-variables-in-classes)

    // ── Shutdown — cleanup GPU resources ──
    void Shutdown();

public:
    BaseTechnique() = default;

    // ── Engine set usage (sets 0-3 are always bound at layout slots 0-3) ──
    // Custom bindings start at set 4. No opt-in needed for engine sets.

    // ── Declare a PerMaterial binding ──
    // T is the C++ data type; set/binding are user-specified (per-technique scope).
    // Debug assert fires if set+binding already declared within this technique.
    template<typename T>
    void DeclarePerMaterial(const std::uint32_t set, const std::uint32_t binding) {
        ValidateNoBindingCollision(set, binding);
        const BindingDecl decl{set, binding, BindingKind::PerMaterial, sizeof(T), std::type_index(typeid(T))};
        DeclareBindingImpl(decl);
    }

    // ── Declare a Shared binding ──
    template<typename T>
    void DeclareShared(const std::uint32_t set, const std::uint32_t binding) {
        ValidateNoBindingCollision(set, binding);
        const BindingDecl decl{set, binding, BindingKind::Shared, 0, std::type_index(typeid(T))};
        DeclareBindingImpl(decl);
    }

    // ── Set technique ID (called by TechniqueManager during registration) ──
    void SetId(const TechniqueId id) { id_ = id; }

    // ── Compilation (separate from constructor) ──
    // Creates pipeline layout with engine sets 0-3 + custom sets 4+.
    // Builds one BlockArray per PerMaterial binding, one GpuBuffer per Shared binding.
    // Returns false if pipeline creation failed; the technique is then unusable
    // (GetPipeline() returns VK_NULL_HANDLE) and resource setup is skipped.
    [[nodiscard]] bool Compile(VulkanBackend::Vulkan::VulkanBootstrap& bootstrap,
                 ShaderSystem::ShaderManager& shader_mgr,
                 ShaderSystem::PipelineFactory& pipeline_factory,
                 ShaderSystem::ShaderId vert_id,
                 ShaderSystem::ShaderId frag_id,
                 const VulkanEngine::StandardMeshPipeline::PipelineConfig& config,
                 vk::DescriptorSetLayout bindless_layout,
                 vk::DescriptorSetLayout submesh_vertex_layout,
                 vk::DescriptorSetLayout raw_vertex_layout,
                 vk::DescriptorSetLayout indirection_layout,
                 vk::DescriptorSetLayout scene_uniform_layout = nullptr);

    // ── Hot-reload: rebuild the pipeline when a shader version changes ──
    void PollAndRebuild(ShaderSystem::ShaderManager& shaders,
                        ShaderSystem::PipelineFactory& factory,
                        std::uint32_t frame_index);

private:
    TechniqueId id_{};
    std::vector<BindingDecl> bindings_;
    std::vector<VulkanEngine::GpuResources::BlockArray> block_arrays_;      // one per PerMaterial binding
    std::vector<VulkanEngine::GpuResources::GpuBuffer> shared_buffers_;      // one per Shared binding
    std::vector<std::vector<std::byte>> shared_cpu_data_;                    // one per Shared binding (technique-local)

    // Per-material descriptor arrays: one entry per PerMaterial binding, tracking
    // where its array lives and which blocks have had a descriptor written.
    struct MaterialArrayBinding {
        std::size_t binding_index = 0;      // index into bindings_ / GetBindings()
        std::size_t block_array_index = 0;  // PerMaterial ordinal into block_arrays_
        vk::DescriptorSet set = nullptr;    // custom set that owns this binding
        std::uint32_t binding_number = 0;   // descriptor binding number within that set
        std::vector<std::uint8_t> block_bound;  // 1 once the block's descriptor is written
    };
    std::vector<MaterialArrayBinding> material_array_bindings_;
    const vk::raii::Device* device_ = nullptr;  // backend device, valid for the technique's lifetime

    vk::raii::PipelineLayout pipeline_layout_ = nullptr;
    ShaderSystem::PipelineSlot pipeline_slot_;

    // Hot-reload state: the fully-built pipeline desc is stored as a member so
    // its pointer-bearing fields (color_blend.pAttachments) stay valid for the
    // technique's lifetime. The desc is rebuilt from config in Compile() and
    // re-fed to PipelineFactory when a shader version changes.
    ShaderSystem::ShaderId vert_id_{};
    ShaderSystem::ShaderId frag_id_{};
    bool compiled_ = false;
    vk::PipelineColorBlendAttachmentState color_blend_attachment_{};
    ShaderSystem::GraphicsPipelineDesc pipeline_desc_{};

    // Descriptor pool + sets for custom bindings (sets 4+)
    vk::raii::DescriptorPool descriptor_pool_ = nullptr;
    std::vector<vk::raii::DescriptorSet> custom_descriptor_sets_;
    std::vector<vk::DescriptorSet> custom_descriptor_set_handles_;  // non-owning views for accessor
    std::vector<vk::raii::DescriptorSetLayout> custom_set_layouts_;  // was local in Compile() — fixes lifetime bug

    void DeclareBindingImpl(BindingDecl decl);
    void ValidateNoBindingCollision(std::uint32_t set, std::uint32_t binding) const;

    // Group bindings by set number for descriptor set layout creation
    struct BindingGroup {
        std::uint32_t set;
        std::vector<const BindingDecl*> bindings;
    };
    [[nodiscard]] std::vector<BindingGroup> GroupBindingsBySet() const;
};

} // namespace VulkanEngine::TechniqueManager
