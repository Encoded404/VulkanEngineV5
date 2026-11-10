module;

export module VulkanEngine.Mesh.SkeletalAnimation;

import std;

export import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.GpuResources.MeshData;

// Skeletal asset evaluation, device-free.
//
// This module owns the *behaviour* of the asset-side types declared in
// MeshTypes: sampling an animation clip, composing a world pose, building the
// skin palette, and quantizing weights for the GPU stream. It never touches a
// Vulkan device, so the whole animation data path is testable without one.
//
// Layering: MeshTypes is the leaf; this module imports it. MeshTypes must never
// import SkeletalAnimation (a C++20 module import cycle is ill-formed).
//
// ── Matrix convention ───────────────────────────────────────────────────────
// `SubMeshTransform` converts to a *column-major* math matrix with FromTRS.
// `SkeletonRoot::transform`, `Skin::inverse_bind`, the `world` arrays and the
// product M = world[skin.joints[k]] * inverse_bind[k] all use that column-major
// layout (offset c*4 + r holds row r, column c).
//
// `ComputePalette` writes the same bytes unchanged into `out_palette`. Read as a
// row-major matrix — which is how Slang interprets a `float4x4` in a storage
// buffer by default — that array is transpose(M). The deform shader therefore
// uses `mul(float4(pos, 1), m)`, which computes p * transpose(M) = M * p for the
// column-vector M. This is the row-vector convention `expand.slang`'s
// `modelMat` already uses. Do not pre-transpose the array: doing so and keeping
// the row-vector multiply would apply M transposed.

export namespace VulkanEngine {

    // Column-major T * R * S. The quaternion is xyzw and is normalized here; a
    // degenerate (zero-length) quaternion is treated as identity.
    [[nodiscard]] std::array<float, 16> FromTRS(const SubMeshTransform& trs);

    // Fills `out_local` from `SkeletonJoint::local_bind`, then overwrites only the
    // clip's channelled joints. A clip switch therefore restores the bind pose for
    // joints the new clip does not animate. Time wraps against `clip.duration`.
    // Rotation channels interpolate with shortest-arc slerp and store a unit
    // quaternion; translation/scale channels lerp or step.
    void SampleClip(const AnimationClip& clip,
                    const Skeleton& skeleton,
                    float time,
                    std::span<SubMeshTransform> out_local);

    // Composes world transforms from the skeleton hierarchy. Order-independent:
    // glTF `skin.joints` may store a child before its parent, so only
    // `SkeletonJoint::parent` and `Skeleton::roots` drive the traversal. Roots
    // start at their `SkeletonRoot::transform`; a joint not reachable from any
    // root stays identity and is reported once.
    void ComputeWorld(const Skeleton& skeleton,
                      std::span<const SubMeshTransform> local,
                      std::span<std::array<float, 16>> out_world);

    // out_palette[k] = world[skin.joints[k]] * inverse_bind[k], column-major.
    // A skin joint index outside `world` (malformed asset) leaves the entry at
    // identity rather than reading out of bounds.
    void ComputePalette(const Skin& skin,
                        std::span<const std::array<float, 16>> world,
                        std::span<std::array<float, 16>> out_palette);

    // Identity animation: composes the skeleton's bind pose into `out_world`.
    void ResolveBindPose(const Skeleton& skeleton,
                         std::span<std::array<float, 16>> out_world);

    // Drops weights below epsilon, normalizes the survivors across all 8
    // influences and quantizes them to u8 summing to exactly 255. The values are
    // written back as q / 255.0f, so `round(value * 255)` recovers q exactly.
    // Returns false when every weight is zero (the vertex is rigid/bind pose);
    // the array is then left all-zero.
    [[nodiscard]] bool NormalizeAndQuantizeWeights(std::array<float, 8>& in_out);

    // A self-contained copy of the animation data of a mesh. `MeshData`'s
    // skeletons/skins/animations live in `MeshRegistry::LoadedMeshInfo::cpu_data`,
    // which is replaced on re-registration and removed on eviction, so a
    // component must never alias them. A `SkeletonPose`/`AnimationPlayer` pair
    // holds one shared `SkeletalAsset` instead.
    struct SkeletalAsset
    {
        std::vector<Skeleton> skeletons;
        std::vector<Skin> skins;
        std::vector<AnimationClip> animations;
    };

    // Copies the asset-side vectors out of MeshData.
    [[nodiscard]] std::shared_ptr<const SkeletalAsset> MakeSkeletalAsset(const GpuResources::MeshData& data);

} // namespace VulkanEngine
