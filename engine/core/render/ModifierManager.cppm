module;

export module VulkanEngine.ModifierManager;

import std;

export import VulkanEngine.ShaderManager;
export import VulkanEngine.ModifierId;

// Registry of vertex-modifier compute kernels.
//
// A modifier is a `[numthreads(N,1,1)]` compute shader that reads a source vertex
// region and writes an equally sized destination region (the stack is strictly
// 1:1 in vertices). This module is pure bookkeeping: it owns no device object and
// builds no pipeline, so registration is device-free and testable without a
// Vulkan device. A renderer resolves the shader ids it finds here into compute
// pipelines once per frame.
//
// Main thread only, between frames. Registering or unregistering during a frame's
// gather-to-submit window would invalidate the frame's dispatch list.

export namespace VulkanEngine::Modifiers {

struct ModifierDesc {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::string name;
    // Reflected compute shader id from ShaderManager. kInvalidShaderId is
    // rejected by Register: an entry that names no shader can never be built.
    ShaderSystem::ShaderId compute_shader = ShaderSystem::kInvalidShaderId;
    std::string entry_point = "main";
    // Bytes of per-instance parameter data the shader reads through
    // `modifierParams`. 0 for a modifier with no parameters (the builtin skinning
    // pass).
    std::uint32_t param_size = 0;
    // Required alignment of the parameter block inside the flat parameter buffer.
    // Power of two in [4, 256].
    std::uint32_t param_alignment = 16;
    // Shader [numthreads] width. The dispatch rounds vertex_count up to a whole
    // multiple of this, so the shader must early-out threads past vertex_count.
    std::uint32_t workgroup_size = 64;
    // Informational until a consumer exists: they describe what a modifier does
    // to the vertex stream, not how it is dispatched.
    bool writes_normals = false;
    bool writes_tangents = false;
    bool may_change_bounds = true;
    // Set by RegisterBuiltin, cleared by Register. A builtin cannot be
    // unregistered.
    bool builtin = false;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

class ModifierManager {
public:
    static constexpr std::uint32_t kMaxParamBytes = 4096;
    static constexpr std::uint32_t kMaxWorkgroupSize = 1024;
    // The builtin entry that carries linear-blend skinning. Registered by the
    // renderer before its pipelines are created.
    static constexpr std::string_view kSkinningName = "skinning";

    struct Entry {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        ModifierId id{};
        ModifierDesc desc{};
        bool alive = false;
        // NOLINTEND(misc-non-private-member-variables-in-classes)
    };

    // Validates and inserts an app modifier. Returns kInvalidModifierId and logs
    // on a validation failure. `desc.builtin` is forced to false.
    ModifierId Register(ModifierDesc desc);

    // Validates and inserts a builtin. A descriptor named "skinning" becomes the
    // manager's SkinningId(); a second builtin with that name is rejected.
    ModifierId RegisterBuiltin(ModifierDesc desc);

    // Retires an entry. Builtins cannot be removed (logged and ignored). The slot
    // is recycled by a later Register with an incremented generation.
    void Unregister(ModifierId id);

    // Clears the registry, including the builtin. Called on engine shutdown so a
    // later initialization registers skinning exactly once.
    void Reset();

    [[nodiscard]] const ModifierDesc* Get(ModifierId id) const;
    [[nodiscard]] bool IsAlive(ModifierId id) const;
    // The live and retired entries. The view is stable until the next mutation;
    // never keep a ModifierDesc pointer across Register/Unregister/Reset.
    [[nodiscard]] std::span<const Entry> Entries() const { return entries_; }
    // Bumped by every successful registration, builtin registration and
    // unregistration. A renderer reconciles its pipelines when this changes.
    [[nodiscard]] std::uint64_t RegistryVersion() const { return registry_version_; }
    [[nodiscard]] ModifierId SkinningId() const { return skinning_id_; }

private:
    // True when `desc` passes the shared registration rules. Logs the reason on
    // failure.
    [[nodiscard]] static bool Validate(const ModifierDesc& desc);
    ModifierId Insert(ModifierDesc desc);

    std::vector<Entry> entries_;
    std::vector<std::uint32_t> free_slots_;
    ModifierId skinning_id_{};
    std::uint64_t registry_version_ = 0;
};

} // namespace VulkanEngine::Modifiers
