module;

export module VulkanEngine.TechniqueManager.BaseTechnique;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.TechniqueManager.TechniqueId;
export import VulkanEngine.GpuResources.BlockArray;
export import VulkanEngine.GpuResources.GpuBuffer;
export import VulkanEngine.GpuResources.StagingManager;

export namespace VulkanEngine::TechniqueManager {

// ── BaseTechnique — abstract base for all rendering techniques ──
export class BaseTechnique {
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

    TechniqueId GetId() const { return id_; }
    std::span<const BindingDecl> GetBindings() const { return bindings_; }
    std::size_t GetBindingCount() const { return bindings_.size(); }
    const BindingDecl& GetBinding(std::size_t i) const { return bindings_[i]; }

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
        for (std::size_t i = 0; i < bindings_.size(); ++i) {
            if (bindings_[i].kind == BindingKind::PerMaterial &&
                bindings_[i].type_index == std::type_index(typeid(T))) {
                assert(i < block_arrays_.size());
                return &block_arrays_[i];
            }
        }
        assert(!"PerMaterial binding type not found — did you declare it?");
        return nullptr;
    }

    // ── Block array access by binding index ──
    VulkanEngine::GpuResources::BlockArray* GetBlockArray(std::size_t binding_index) {
        if (binding_index < block_arrays_.size()) {
            return &block_arrays_[binding_index];
        }
        return nullptr;
    }

    // ── GPU resource access ──
    vk::Pipeline GetPipeline() const { return pipeline_ ? **pipeline_ : nullptr; }
    vk::PipelineLayout GetPipelineLayout() const { return pipeline_layout_ ? **pipeline_layout_ : nullptr; }

    // ── Shutdown — cleanup GPU resources ──
    void Shutdown();

protected:
    BaseTechnique() = default;

    // ── Engine set usage (sets 0-3 are always bound at layout slots 0-3) ──
    // Custom bindings start at set 4. No opt-in needed for engine sets.

    // ── Declare a PerMaterial binding ──
    // T is the C++ data type; set/binding are user-specified (per-technique scope).
    // Debug assert fires if set+binding already declared within this technique.
    template<typename T>
    void DeclarePerMaterial(std::uint32_t set, std::uint32_t binding) {
        ValidateNoBindingCollision(set, binding);
        BindingDecl decl{set, binding, BindingKind::PerMaterial, sizeof(T), std::type_index(typeid(T))};
        DeclareBindingImpl(decl);
    }

    // ── Declare a Shared binding ──
    template<typename T>
    void DeclareShared(std::uint32_t set, std::uint32_t binding) {
        ValidateNoBindingCollision(set, binding);
        BindingDecl decl{set, binding, BindingKind::Shared, 0, std::type_index(typeid(T))};
        DeclareBindingImpl(decl);
    }

    // ── Compilation (separate from constructor) ──
    // Creates pipeline layout with engine sets 0-3 + custom sets 4+.
    // Builds one BlockArray per PerMaterial binding, one GpuBuffer per Shared binding.
    void Compile(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                 std::span<const std::uint32_t> vert_spv,
                 std::span<const std::uint32_t> frag_spv,
                 const VulkanEngine::StandardMeshPipeline::PipelineConfig& config,
                 vk::DescriptorSetLayout bindless_layout,
                 vk::DescriptorSetLayout submesh_vertex_layout,
                 vk::DescriptorSetLayout raw_vertex_layout,
                 vk::DescriptorSetLayout indirection_layout);

    friend class VulkanEngine::TechniqueManager::TechniqueManager;

private:
    TechniqueId id_{};
    std::vector<BindingDecl> bindings_;
    std::vector<VulkanEngine::GpuResources::BlockArray> block_arrays_;      // one per PerMaterial binding
    std::vector<VulkanEngine::GpuResources::GpuBuffer> shared_buffers_;      // one per Shared binding
    std::vector<std::vector<std::byte>> shared_cpu_data_;                    // one per Shared binding (technique-local)

    vk::raii::PipelineLayout pipeline_layout_ = nullptr;
    vk::raii::Pipeline pipeline_ = nullptr;

    void DeclareBindingImpl(BindingDecl decl);
    void ValidateNoBindingCollision(std::uint32_t set, std::uint32_t binding) const;

    // Group bindings by set number for descriptor set layout creation
    struct BindingGroup {
        std::uint32_t set;
        std::vector<const BindingDecl*> bindings;
    };
    std::vector<BindingGroup> GroupBindingsBySet() const;
};

} // namespace VulkanEngine::TechniqueManager
