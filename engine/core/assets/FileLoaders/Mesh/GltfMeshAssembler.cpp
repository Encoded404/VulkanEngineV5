module;

#include <glm/glm.hpp> // NOLINT(misc-include-cleaner) — ShapeDesc point/box vectors
#include <logging/logging_macros.hpp>

module VulkanEngine.FileLoaders.Mesh.GltfMeshAssembler;

import std;

import logiface;
import fastgltf;

import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.Mesh.SkeletalAnimation;
import VulkanEngine.Physics.Types;
import VulkanEngine.FileLoaders.Mesh.MeshLoaderBase;
import VulkanEngine.FileLoaders.Mesh.GltfMaterialImport;
import VulkanEngine.MaterialManager.MaterialId;
import VulkanEngine.Mesh.TangentGenerator;

namespace VulkanEngine::FileLoaders::Mesh {

namespace {

// ── fastgltf values -> engine values ────────────────────────────────────────

[[nodiscard]] MeshVertexVec3 ToVec3(const fastgltf::math::fvec3& v) {
    return MeshVertexVec3{v.x(), v.y(), v.z()};
}

[[nodiscard]] MeshVertexVec2 ToVec2(const fastgltf::math::fvec2& v) {
    return MeshVertexVec2{v.x(), v.y()};
}

[[nodiscard]] SubMeshTransform ToSubMeshTransform(const fastgltf::TRS& trs) {
    SubMeshTransform out{};
    out.position = ToVec3(trs.translation);
    out.rotation[0] = trs.rotation.x();
    out.rotation[1] = trs.rotation.y();
    out.rotation[2] = trs.rotation.z();
    out.rotation[3] = trs.rotation.w();
    out.scale = ToVec3(trs.scale);
    return out;
}

// A node's local transform as the engine's TRS. A matrix node is decomposed,
// which is exact for the shear-free matrices the glTF spec requires; anything
// else is reported by HasShear on the accumulated world matrix.
[[nodiscard]] SubMeshTransform LocalTrs(const fastgltf::Node& node) {
    if (const auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
        return ToSubMeshTransform(*trs);
    }
    const auto& matrix = std::get<fastgltf::math::fmat4x4>(node.transform);
    fastgltf::math::fvec3 scale, translation;
    fastgltf::math::fquat rotation;
    fastgltf::math::decomposeTransformMatrix(matrix, scale, rotation, translation);

    SubMeshTransform out{};
    out.position = ToVec3(translation);
    out.rotation[0] = rotation.x();
    out.rotation[1] = rotation.y();
    out.rotation[2] = rotation.z();
    out.rotation[3] = rotation.w();
    out.scale = ToVec3(scale);
    return out;
}

// The same local transform as a matrix, for the accumulated world matrix the
// shear test needs. Translation, then rotation, then scale: T * R * S.
[[nodiscard]] fastgltf::math::fmat4x4 LocalMatrix(const fastgltf::Node& node) {
    if (const auto* matrix = std::get_if<fastgltf::math::fmat4x4>(&node.transform)) {
        return *matrix;
    }
    const auto& trs = std::get<fastgltf::TRS>(node.transform);
    fastgltf::math::fmat4x4 m;
    m = fastgltf::math::translate(m, trs.translation);
    m = fastgltf::math::rotate(m, trs.rotation);
    m = fastgltf::math::scale(m, trs.scale);
    return m;
}

// Reads one accessor into a plain vector (defined below; the skeletal builders
// use it before that definition appears).
template <typename T>
[[nodiscard]] std::vector<T> ReadAccessor(const fastgltf::Asset& asset, std::size_t accessor_index);

// ── skeletal import ─────────────────────────────────────────────────────────

constexpr std::size_t kNoNode = std::numeric_limits<std::size_t>::max();

[[nodiscard]] constexpr std::array<float, 16> IdentityMat() noexcept {
    return {1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
}

// fastgltf stores matrices column-major, which is exactly the convention the
// engine's Skeleton/Skin/Pose arrays use: copy the 16 floats verbatim.
[[nodiscard]] std::array<float, 16> ToColumnMajor(const fastgltf::math::fmat4x4& m) {
    std::array<float, 16> out{};
    std::copy_n(m.data(), 16, out.begin());
    return out;
}

[[nodiscard]] std::vector<std::size_t> BuildParentMap(const fastgltf::Asset& asset) {
    std::vector<std::size_t> parents(asset.nodes.size(), kNoNode);
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        for (const std::size_t child : asset.nodes[i].children) {
            if (child < parents.size() && parents[child] == kNoNode) parents[child] = i;
        }
    }
    return parents;
}

// Memoized world transforms from the scene root. A skin's inverse-bind matrices
// are expressed in the space below the armature node, so the accumulated
// transform from the root down to a root joint's *parent* is what starts a
// SkeletonRoot. Using a shallower ancestor (for example only `skin.skeleton`)
// would drop intermediate node transforms between it and the joint.
class NodeWorldCache
{
public:
    NodeWorldCache(const fastgltf::Asset& asset, std::vector<std::size_t> parents)
        : asset_(asset), parents_(std::move(parents)), world_(asset.nodes.size()) {}

    [[nodiscard]] fastgltf::math::fmat4x4 World(const std::size_t node) {
        if (node >= world_.size()) return fastgltf::math::fmat4x4{};
        if (world_[node].has_value()) return *world_[node];

        std::vector<std::size_t> chain;
        std::size_t current = node;
        while (current != kNoNode && current < world_.size() && !world_[current].has_value()) {
            chain.push_back(current);
            current = parents_[current];
        }
        fastgltf::math::fmat4x4 accumulated =
            (current != kNoNode && current < world_.size()) ? *world_[current]
                                                            : fastgltf::math::fmat4x4{};
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            accumulated = accumulated * LocalMatrix(asset_.nodes[*it]);
            world_[*it] = accumulated;
        }
        return world_[node].value_or(fastgltf::math::fmat4x4{});
    }

private:
    const fastgltf::Asset& asset_;
    std::vector<std::size_t> parents_;
    std::vector<std::optional<fastgltf::math::fmat4x4>> world_;
};

// Which skeleton/joint(s) a glTF node drives. glTF animation channels target
// scene nodes, not joint indices, so this map is what turns a channel into
// SampleClip-safe (skeleton, joint) pairs. A node can belong to several
// skeletons when their joint lists overlap without being identical (for example
// [spine, neck, head] and [spine, neck]); each of those skeletons needs the
// channel, or the entity posed by the later skeleton would stay in bind pose.
struct SkinImport
{
    std::unordered_map<std::size_t,
                       std::vector<std::pair<std::uint32_t, std::uint32_t>>> node_to_joint;
};

// Builds Mesh::skeletons and Mesh::skins from asset.skins. Skeletons are deduped
// by their ordered joint-node list: two skins over the same joints share one
// Skeleton, so a single SkeletonPose (one skeleton_index) covers both. Two
// genuinely different joint sets stay two skeletons.
[[nodiscard]] SkinImport BuildSkeletonsAndSkins(const fastgltf::Asset& asset,
                                                const std::vector<std::size_t>& parents,
                                                NodeWorldCache& world,
                                                VulkanEngine::Mesh& mesh) {
    SkinImport imported;
    mesh.skins.resize(asset.skins.size());
    std::map<std::vector<std::size_t>, std::uint32_t> by_joint_nodes;

    for (std::size_t skin_index = 0; skin_index < asset.skins.size(); ++skin_index) {
        const fastgltf::Skin& source = asset.skins[skin_index];
        // The skin's joint list is positional: JOINTS_n values index it and the
        // inverse-bind matrices align with it. Dropping an invalid entry would
        // shift every later joint and mis-bind the whole skin, so a skin with any
        // invalid joint node is rejected whole instead.
        std::vector<std::size_t> joint_nodes;
        joint_nodes.reserve(source.joints.size());
        std::optional<std::size_t> invalid_joint;
        for (const std::size_t joint_node : source.joints) {
            if (joint_node >= asset.nodes.size()) {
                invalid_joint = joint_node;
                break;
            }
            joint_nodes.push_back(joint_node);
        }
        if (invalid_joint.has_value()) {
            LOGIFACE_LOG(warn, "GltfMeshAssembler: skin " + std::to_string(skin_index) +
                                   " references joint node " + std::to_string(*invalid_joint) +
                                   ", which does not exist; the skin is not imported");
            continue;
        }
        if (joint_nodes.empty()) {
            LOGIFACE_LOG(warn, "GltfMeshAssembler: skin " + std::to_string(skin_index) +
                                   " has no valid joints; it is imported empty");
            continue;
        }
        if (joint_nodes.size() > std::numeric_limits<std::uint16_t>::max()) {
            LOGIFACE_LOG(warn, "GltfMeshAssembler: skin " + std::to_string(skin_index) +
                                   " has " + std::to_string(joint_nodes.size()) +
                                   " joints; at most 65535 are addressable, so the skin is not imported");
            continue;
        }

        std::uint32_t skeleton_index = kNoSkeleton;
        if (const auto found = by_joint_nodes.find(joint_nodes); found != by_joint_nodes.end()) {
            skeleton_index = found->second;
        } else {
            skeleton_index = static_cast<std::uint32_t>(mesh.skeletons.size());
            VulkanEngine::Skeleton skeleton;
            skeleton.joints.resize(joint_nodes.size());
            skeleton.names.reserve(joint_nodes.size());
            std::unordered_map<std::size_t, std::uint32_t> joint_of_node;
            for (std::uint32_t j = 0; j < joint_nodes.size(); ++j) {
                joint_of_node.emplace(joint_nodes[j], j);
                skeleton.joints[j].local_bind = LocalTrs(asset.nodes[joint_nodes[j]]);
                const auto& name = asset.nodes[joint_nodes[j]].name;
                skeleton.names.emplace_back(name.data(), name.size());
            }
            for (std::uint32_t j = 0; j < joint_nodes.size(); ++j) {
                const std::size_t parent_node = parents[joint_nodes[j]];
                const auto parent_joint = (parent_node == kNoNode)
                    ? joint_of_node.end()
                    : joint_of_node.find(parent_node);
                if (parent_joint != joint_of_node.end()) {
                    skeleton.joints[j].parent = parent_joint->second;
                    continue;
                }
                VulkanEngine::SkeletonRoot root{};
                root.joint = j;
                root.transform = (parent_node == kNoNode)
                    ? IdentityMat()
                    : ToColumnMajor(world.World(parent_node));
                skeleton.roots.push_back(root);
            }
            mesh.skeletons.push_back(std::move(skeleton));
            by_joint_nodes.emplace(joint_nodes, skeleton_index);
            for (std::uint32_t j = 0; j < joint_nodes.size(); ++j) {
                imported.node_to_joint[joint_nodes[j]].emplace_back(skeleton_index, j);
            }
        }

        VulkanEngine::Skin& skin = mesh.skins[skin_index];
        skin.skeleton = skeleton_index;
        // The skeleton's joint array was built from this exact ordered node list,
        // so the skin-local -> skeleton-joint map is the identity.
        skin.joints.resize(joint_nodes.size());
        for (std::uint32_t k = 0; k < joint_nodes.size(); ++k) {
            skin.joints[k] = static_cast<std::uint16_t>(k);
        }
        skin.inverse_bind.assign(joint_nodes.size(), IdentityMat());
        if (source.inverseBindMatrices.has_value() &&
            *source.inverseBindMatrices < asset.accessors.size() &&
            asset.accessors[*source.inverseBindMatrices].type == fastgltf::AccessorType::Mat4) {
            const std::vector<fastgltf::math::fmat4x4> inverse_bind =
                ReadAccessor<fastgltf::math::fmat4x4>(asset, *source.inverseBindMatrices);
            const std::size_t count = std::min(inverse_bind.size(), skin.inverse_bind.size());
            for (std::size_t k = 0; k < count; ++k) {
                skin.inverse_bind[k] = ToColumnMajor(inverse_bind[k]);
            }
            if (inverse_bind.size() < skin.inverse_bind.size()) {
                LOGIFACE_LOG(warn, "GltfMeshAssembler: skin " + std::to_string(skin_index) +
                                       " has fewer inverse-bind matrices than joints; the missing "
                                       "entries fall back to identity");
            }
        } else {
            LOGIFACE_LOG(warn, "GltfMeshAssembler: skin " + std::to_string(skin_index) +
                                   " has no usable (MAT4) inverseBindMatrices accessor; identity "
                                   "matrices are used");
        }
    }
    return imported;
}

// Builds Mesh::animations from asset.animations. A glTF channel targets a scene
// node; it is resolved to (skeleton, joint) through the skin import map and
// dropped when its node is not a joint of any imported skeleton. An animation
// that targets several skeletons is split into one clip per skeleton, named
// `<name>#<skeleton-index>`, so SampleClip indexes `out_local[joint]` safely.
void BuildAnimations(const fastgltf::Asset& asset,
                     const SkinImport& imported,
                     VulkanEngine::Mesh& mesh) {
    for (const fastgltf::Animation& source : asset.animations) {
        std::vector<VulkanEngine::AnimationClip> groups(mesh.skeletons.size());
        std::vector<bool> used(mesh.skeletons.size(), false);
        bool warned_non_joint = false;
        bool warned_cubic = false;
        bool warned_weights = false;

        for (const fastgltf::AnimationChannel& channel : source.channels) {
            if (channel.path == fastgltf::AnimationPath::Weights) {
                if (!warned_weights) {
                    warned_weights = true;
                    LOGIFACE_LOG(warn, "GltfMeshAssembler: animation '" +
                                           std::string(source.name.c_str()) +
                                           "' animates morph-target weights, which are not "
                                           "imported; those channels are dropped");
                }
                continue;
            }
            if (!channel.nodeIndex.has_value()) continue;
            const auto found = imported.node_to_joint.find(*channel.nodeIndex);
            if (found == imported.node_to_joint.end()) {
                if (!warned_non_joint) {
                    warned_non_joint = true;
                    LOGIFACE_LOG(warn, "GltfMeshAssembler: animation '" +
                                           std::string(source.name.c_str()) + "' targets node " +
                                           std::to_string(*channel.nodeIndex) +
                                           ", which is not a joint of any imported skin; the "
                                           "channel is dropped");
                }
                continue;
            }
            if (channel.samplerIndex >= source.samplers.size()) continue;
            const fastgltf::AnimationSampler& sampler = source.samplers[channel.samplerIndex];
            if (sampler.inputAccessor >= asset.accessors.size() ||
                sampler.outputAccessor >= asset.accessors.size()) {
                continue;
            }
            if (asset.accessors[sampler.inputAccessor].type != fastgltf::AccessorType::Scalar) {
                continue;
            }

            const bool cubic = sampler.interpolation == fastgltf::AnimationInterpolation::CubicSpline;
            if (cubic && !warned_cubic) {
                warned_cubic = true;
                LOGIFACE_LOG(warn, "GltfMeshAssembler: animation '" +
                                       std::string(source.name.c_str()) +
                                       "' uses cubic-spline interpolation; its keys keep the "
                                       "spline value (the middle output of each triple) and are "
                                       "stored as linear");
            }

            VulkanEngine::AnimationChannel out;
            out.interpolation = (sampler.interpolation == fastgltf::AnimationInterpolation::Step)
                ? VulkanEngine::AnimationInterpolation::Step
                : VulkanEngine::AnimationInterpolation::Linear;
            switch (channel.path) {
                case fastgltf::AnimationPath::Translation:
                    out.path = VulkanEngine::AnimationPath::Translation;
                    break;
                case fastgltf::AnimationPath::Rotation:
                    out.path = VulkanEngine::AnimationPath::Rotation;
                    break;
                case fastgltf::AnimationPath::Scale:
                    out.path = VulkanEngine::AnimationPath::Scale;
                    break;
                default:
                    continue;
            }
            // The accessor element type must match the path: rotation output is
            // VEC4, translation/scale output is VEC3. ReadAccessor asserts this in
            // a debug build and over-reads without asserts, so reject mismatches.
            const fastgltf::AccessorType expected_type =
                (out.path == VulkanEngine::AnimationPath::Rotation)
                    ? fastgltf::AccessorType::Vec4
                    : fastgltf::AccessorType::Vec3;
            if (asset.accessors[sampler.outputAccessor].type != expected_type) {
                continue;
            }

            const std::vector<float> times = ReadAccessor<float>(asset, sampler.inputAccessor);
            if (times.empty()) continue;

            if (out.path == VulkanEngine::AnimationPath::Rotation) {
                const std::vector<fastgltf::math::fvec4> values =
                    ReadAccessor<fastgltf::math::fvec4>(asset, sampler.outputAccessor);
                out.keys.reserve(times.size());
                for (std::size_t k = 0; k < times.size(); ++k) {
                    const std::size_t value_index = cubic ? 3U * k + 1U : k;
                    if (value_index >= values.size()) break;
                    VulkanEngine::AnimationKey key{};
                    key.time = std::max(0.0f, times[k]);
                    key.value = {values[value_index].x(), values[value_index].y(),
                                 values[value_index].z(), values[value_index].w()};
                    out.keys.push_back(key);
                }
            } else {
                const std::vector<fastgltf::math::fvec3> values =
                    ReadAccessor<fastgltf::math::fvec3>(asset, sampler.outputAccessor);
                out.keys.reserve(times.size());
                for (std::size_t k = 0; k < times.size(); ++k) {
                    const std::size_t value_index = cubic ? 3U * k + 1U : k;
                    if (value_index >= values.size()) break;
                    VulkanEngine::AnimationKey key{};
                    key.time = std::max(0.0f, times[k]);
                    key.value = {values[value_index].x(), values[value_index].y(),
                                 values[value_index].z(), 0.0f};
                    out.keys.push_back(key);
                }
            }
            if (out.keys.empty()) continue;
            std::ranges::sort(out.keys, [](const VulkanEngine::AnimationKey& a,
                                           const VulkanEngine::AnimationKey& b) {
                return a.time < b.time;
            });
            // Emit one channel per skeleton that owns the target node. For the
            // common single-skeleton case this is exactly one copy.
            for (const auto& [skeleton_index, joint] : found->second) {
                VulkanEngine::AnimationChannel channel_copy = out; // shared key stream
                channel_copy.joint = joint;
                groups[skeleton_index].channels.push_back(std::move(channel_copy));
                used[skeleton_index] = true;
            }
        }

        std::size_t used_count = 0;
        for (const bool entry : used) used_count += entry ? 1U : 0U;
        const std::string base_name = source.name.empty() ? std::string("animation")
                                                          : std::string(source.name.c_str());
        for (std::size_t skeleton_index = 0; skeleton_index < groups.size(); ++skeleton_index) {
            if (!used[skeleton_index]) continue;
            VulkanEngine::AnimationClip& clip = groups[skeleton_index];
            clip.skeleton = static_cast<std::uint32_t>(skeleton_index);
            clip.name = (used_count > 1)
                ? base_name + "#" + std::to_string(skeleton_index)
                : base_name;
            for (const VulkanEngine::AnimationChannel& channel : clip.channels) {
                if (!channel.keys.empty()) {
                    clip.duration = std::max(clip.duration, channel.keys.back().time);
                }
            }
            mesh.animations.push_back(std::move(clip));
        }
    }
}

// Shear test for an accumulated world matrix. The engine's TRS placement (and
// the expand pass's closed-form normal matrix) is exact only for R*S; a sheared
// matrix shows up as non-orthogonal axes, which this checks directly. Testing
// orthogonality rather than round-tripping decomposeTransformMatrix matters:
// that decomposition reconstructs positive scales, so it would report a false
// positive on a mirrored node, whose axes are still orthogonal.
[[nodiscard]] bool HasShear(const fastgltf::math::fmat4x4& m, const float epsilon = 1e-4f) {
    std::array<std::array<float, 3>, 3> axis{};
    for (std::size_t c = 0; c < 3; ++c) {
        float length = 0.0f;
        for (std::size_t r = 0; r < 3; ++r) length += m[c][r] * m[c][r];
        length = std::sqrt(length);
        if (length <= 1e-8f) return true; // a degenerate axis cannot be represented
        for (std::size_t r = 0; r < 3; ++r) axis[c][r] = m[c][r] / length;
    }
    for (std::size_t a = 0; a < 3; ++a) {
        for (std::size_t b = a + 1; b < 3; ++b) {
            float dot = 0.0f;
            for (std::size_t r = 0; r < 3; ++r) dot += axis[a][r] * axis[b][r];
            if (std::abs(dot) > epsilon) return true;
        }
    }
    return false;
}

// Reads one accessor into a plain vector. iterateAccessor resolves the accessor's
// component type, normalization and sparseness, so normalized integers,
// KHR_mesh_quantization and sparse accessors all arrive as floats here.
template <typename T>
[[nodiscard]] std::vector<T> ReadAccessor(const fastgltf::Asset& asset, const std::size_t accessor_index) {
    std::vector<T> values;
    const auto& accessor = asset.accessors[accessor_index];
    values.reserve(accessor.count);
    fastgltf::iterateAccessor<T>(asset, accessor, [&values](const T value) { values.push_back(value); });
    return values;
}

// ── KHR_physics_rigid_bodies -> engine physics hints ────────────────────────
//
// The extension attaches a rigid body to a node: a motion (or none, which means
// static), a collider whose geometry is either an implicit shape from
// KHR_implicit_shapes or the mesh of a referenced node, an optional physics
// material index, and an optional trigger form. Everything below only builds
// data (`VulkanEngine::PhysicsHint`); no body is created and no backend type is
// named, matching the assets layer's contract.

using VulkanEngine::Physics::BodyMotionType;
using VulkanEngine::Physics::CollisionLayer;
using VulkanEngine::Physics::ShapeDesc;
using VulkanEngine::Physics::ShapeType;

// Local AABB of every POSITION accessor a glTF mesh's primitives reference.
struct LocalBounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

[[nodiscard]] std::optional<LocalBounds> MeshBounds(const fastgltf::Asset& asset,
                                                    const fastgltf::Mesh& mesh) {
    std::optional<LocalBounds> bounds;
    for (const fastgltf::Primitive& primitive : mesh.primitives) {
        const auto position_it = primitive.findAttribute("POSITION");
        if (position_it == primitive.attributes.end()) continue;
        for (const fastgltf::math::fvec3& p :
             ReadAccessor<fastgltf::math::fvec3>(asset, position_it->accessorIndex)) {
            const glm::vec3 v{p.x(), p.y(), p.z()};
            if (!bounds.has_value()) {
                bounds = LocalBounds{v, v};
            } else {
                bounds->min = glm::min(bounds->min, v);
                bounds->max = glm::max(bounds->max, v);
            }
        }
    }
    return bounds;
}

// The first primitive of a glTF mesh that carries positions, as ShapeDesc data.
// The points stay in mesh-local space, which is the body frame the engine's
// `ShapeDesc` describes; `convex_hull` selects the hull representation, else the
// triangle-mesh one. Nothing here resolves materials or placement: a collider
// shape only needs geometry.
[[nodiscard]] std::optional<ShapeDesc> ShapeFromMeshGeometry(const fastgltf::Asset& asset,
                                                             const std::size_t mesh_index,
                                                             const bool convex_hull) {
    if (mesh_index >= asset.meshes.size()) return std::nullopt;
    for (const fastgltf::Primitive& primitive : asset.meshes[mesh_index].primitives) {
        const auto position_it = primitive.findAttribute("POSITION");
        if (position_it == primitive.attributes.end()) continue;
        const std::vector<fastgltf::math::fvec3> positions =
            ReadAccessor<fastgltf::math::fvec3>(asset, position_it->accessorIndex);
        if (positions.empty()) continue;

        auto points = std::make_shared<std::vector<glm::vec3>>();
        points->reserve(positions.size());
        for (const fastgltf::math::fvec3& p : positions) points->emplace_back(p.x(), p.y(), p.z());

        ShapeDesc shape{};
        shape.points = std::move(points);
        if (convex_hull) {
            shape.type = ShapeType::ConvexHull;
            return shape;
        }
        shape.type = ShapeType::Mesh;
        auto indices = std::make_shared<std::vector<std::uint32_t>>();
        if (primitive.indicesAccessor.has_value()) {
            *indices = ReadAccessor<std::uint32_t>(asset, *primitive.indicesAccessor);
        } else {
            indices->resize(positions.size());
            std::iota(indices->begin(), indices->end(), 0u);
        }
        shape.indices = std::move(indices);
        return shape;
    }
    return std::nullopt;
}

// One KHR_implicit_shapes entry as a ShapeDesc. The engine's capsule and
// cylinder are single-radius; the extension allows a different radius at each
// end, so the larger one is used and the difference is reported.
[[nodiscard]] std::optional<ShapeDesc> ShapeFromImplicitShape(const fastgltf::Asset& asset,
                                                              const std::size_t shape_index) {
    if (shape_index >= asset.shapes.size()) return std::nullopt;
    // `fastgltf::Shape` is the one KHR_implicit_shapes type the parser does not
    // export from its module (the alias is not marked FASTGLTF_EXPORT), so it is
    // reached through `auto` rather than named.
    const auto& source = asset.shapes[shape_index];
    ShapeDesc shape{};
    if (const auto* box = std::get_if<fastgltf::BoxShape>(&source)) {
        shape.type = ShapeType::Box;
        shape.box_extent = glm::vec3{box->size.x(), box->size.y(), box->size.z()};
        return shape;
    }
    if (const auto* sphere = std::get_if<fastgltf::SphereShape>(&source)) {
        shape.type = ShapeType::Sphere;
        shape.radius = static_cast<float>(sphere->radius);
        return shape;
    }
    if (const auto* capsule = std::get_if<fastgltf::CapsuleShape>(&source)) {
        shape.type = ShapeType::Capsule;
        shape.height = static_cast<float>(capsule->height);
        shape.radius = static_cast<float>(std::max(capsule->radiusBottom, capsule->radiusTop));
        if (capsule->radiusBottom != capsule->radiusTop) {
            LOGIFACE_LOG(debug, "GltfMeshAssembler: KHR_implicit_shapes capsule " +
                                    std::to_string(shape_index) + " has unequal radii (" +
                                    std::to_string(capsule->radiusBottom) + " / " +
                                    std::to_string(capsule->radiusTop) +
                                    "); the engine's capsule is single-radius, so the larger is used");
        }
        return shape;
    }
    if (const auto* cylinder = std::get_if<fastgltf::CylinderShape>(&source)) {
        shape.type = ShapeType::Cylinder;
        shape.height = static_cast<float>(cylinder->height);
        shape.radius = static_cast<float>(std::max(cylinder->radiusBottom, cylinder->radiusTop));
        if (cylinder->radiusBottom != cylinder->radiusTop) {
            LOGIFACE_LOG(debug, "GltfMeshAssembler: KHR_implicit_shapes cylinder " +
                                    std::to_string(shape_index) + " has unequal radii (" +
                                    std::to_string(cylinder->radiusBottom) + " / " +
                                    std::to_string(cylinder->radiusTop) +
                                    "); the engine's cylinder is single-radius, so the larger is used");
        }
        return shape;
    }
    return std::nullopt;
}

// The collider's shape, preferring the implicit representation, then the mesh of
// the referenced node, and finally a box around the collider node's own mesh
// bounds. `approximated` reports the last path, which is never presented as an
// exact collider.
[[nodiscard]] std::optional<ShapeDesc> ShapeFromGeometry(const fastgltf::Asset& asset,
                                                         const fastgltf::Node& node,
                                                         const fastgltf::Geometry& geometry,
                                                         bool& approximated) {
    if (geometry.shape.has_value()) {
        if (auto shape = ShapeFromImplicitShape(asset, *geometry.shape)) return shape;
        LOGIFACE_LOG(debug, "GltfMeshAssembler: KHR_physics_rigid_bodies collider references "
                            "KHR_implicit_shapes index " + std::to_string(*geometry.shape) +
                            ", which does not resolve");
    }
    if (geometry.node.has_value()) {
        const std::size_t geometry_node = *geometry.node;
        if (geometry_node < asset.nodes.size() &&
            asset.nodes[geometry_node].meshIndex.has_value()) {
            if (auto shape = ShapeFromMeshGeometry(asset, *asset.nodes[geometry_node].meshIndex,
                                                   geometry.convexHull)) {
                return shape;
            }
        }
        LOGIFACE_LOG(debug, "GltfMeshAssembler: KHR_physics_rigid_bodies collider references node " +
                                std::to_string(geometry_node) +
                                " for mesh geometry, which does not resolve");
    }
    // Nothing authoritative resolved: fall back to the collider node's own mesh
    // bounds. Reported to the caller so the hint records an approximation.
    if (node.meshIndex.has_value() && *node.meshIndex < asset.meshes.size()) {
        if (const auto bounds = MeshBounds(asset, asset.meshes[*node.meshIndex])) {
            approximated = true;
            ShapeDesc shape{};
            shape.type = ShapeType::Box;
            // A flat (or degenerate) mesh still needs a positive box extent.
            shape.box_extent = glm::max(bounds->max - bounds->min, glm::vec3{1e-4f});
            return shape;
        }
    }
    return std::nullopt;
}

// The complete hint for a node carrying the extension: shape, motion, material
// friction/restitution and trigger flag. A node without the extension yields
// nothing, which is what keeps every other asset's submeshes hint-free.
[[nodiscard]] std::optional<VulkanEngine::PhysicsHint> BuildPhysicsHint(
    const fastgltf::Asset& asset, const fastgltf::Node& node) {
    const fastgltf::PhysicsRigidBody* rigid_body = node.physicsRigidBody.get();
    if (rigid_body == nullptr) return std::nullopt;

    VulkanEngine::PhysicsHint hint{};
    // A body with no `motion` is static per the extension's default; the engine
    // has no layer vocabulary in the extension, so every imported body lands on
    // the ordinary geometry layer until gameplay re-assigns it.
    if (rigid_body->motion.has_value()) {
        hint.motion = rigid_body->motion->isKinematic ? BodyMotionType::Kinematic
                                                      : BodyMotionType::Dynamic;
    }
    hint.layer = CollisionLayer::Default;
    hint.sensor = rigid_body->trigger.has_value();

    if (!rigid_body->collider.has_value()) {
        LOGIFACE_LOG(debug, "GltfMeshAssembler: KHR_physics_rigid_bodies body has no collider; "
                            "the default box shape is used");
        return hint;
    }
    const fastgltf::Collider& collider = *rigid_body->collider;
    bool approximated = false;
    if (auto shape = ShapeFromGeometry(asset, node, collider.geometry, approximated)) {
        hint.shape = *shape;
    } else {
        LOGIFACE_LOG(debug, "GltfMeshAssembler: KHR_physics_rigid_bodies collider geometry does not "
                            "resolve to a shape or a mesh; the default box shape is used");
    }
    if (approximated) {
        LOGIFACE_LOG(debug, "GltfMeshAssembler: KHR_physics_rigid_bodies collider geometry could not be "
                            "resolved exactly; it is approximated by a box around the node's mesh bounds");
    }

    if (collider.physicsMaterial.has_value()) {
        const std::size_t index = *collider.physicsMaterial;
        if (index < asset.physicsMaterials.size()) {
            // staticFriction is the engine's coefficient: the engine has one
            // friction value, so the extension's separate dynamic coefficient
            // has nowhere to go.
            hint.friction = static_cast<float>(asset.physicsMaterials[index].staticFriction);
            hint.restitution = static_cast<float>(asset.physicsMaterials[index].restitution);
        } else {
            LOGIFACE_LOG(debug, "GltfMeshAssembler: KHR_physics_rigid_bodies physicsMaterial index " +
                                    std::to_string(index) +
                                    " is out of range; the engine's friction/restitution defaults are used");
        }
    }
    return hint;
}

// ── vertex emission ─────────────────────────────────────────────────────────

// The dedup key: every attribute that is actually consumed. Two vertices merge
// only when they agree on all of them, which is what keeps UV or tangent seams
// split instead of smearing one corner's values across the seam. Skinned
// vertices must also agree on their influences: two corners at the same
// position but bound to different joints are different vertices.
struct VertexKey {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> uv0{};
    std::array<float, 2> uv1{};
    std::array<float, 4> tangent{};
    std::array<std::uint32_t, 8> joints{};  // 4 + 4, zero when unskinned
    std::array<std::uint8_t, 8> weights{};  // quantized, zero when unskinned
    bool has_tangent = false;

    bool operator==(const VertexKey& o) const {
        return position == o.position && normal == o.normal && uv0 == o.uv0 &&
               uv1 == o.uv1 && tangent == o.tangent && joints == o.joints &&
               weights == o.weights && has_tangent == o.has_tangent;
    }
};

struct VertexKeyHash {
    std::size_t operator()(const VertexKey& key) const {
        std::size_t h = 1469598103934665603ull;
        const auto mix = [&h](const std::uint32_t value) {
            h ^= value;
            h *= 1099511628211ull;
        };
        for (const float v : key.position) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            mix(bits);
        }
        for (const float v : key.normal) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            mix(bits);
        }
        for (const float v : key.uv0) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            mix(bits);
        }
        for (const float v : key.uv1) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            mix(bits);
        }
        for (const float v : key.tangent) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            mix(bits);
        }
        for (const std::uint32_t v : key.joints) mix(v);
        for (const std::uint8_t v : key.weights) mix(v);
        mix(key.has_tangent ? 1U : 0U);
        return h;
    }
};

[[nodiscard]] VertexKey MakeKey(const MeshVertexVec3& position,
                                const MeshVertexVec3& normal,
                                const MeshVertexVec2& uv0,
                                const MeshVertexVec2& uv1,
                                const float tangent[4],
                                const std::array<std::uint32_t, 8>& joints,
                                const std::array<std::uint8_t, 8>& weights,
                                const bool has_tangent) {
    VertexKey key{};
    key.position = {position.x, position.y, position.z};
    key.normal = {normal.x, normal.y, normal.z};
    key.uv0 = {uv0.u, uv0.v};
    key.uv1 = {uv1.u, uv1.v};
    key.tangent = {tangent[0], tangent[1], tangent[2], tangent[3]};
    key.joints = joints;
    key.weights = weights;
    key.has_tangent = has_tangent;
    return key;
}

// One primitive's geometry, emitted into the mesh exactly once no matter how
// many nodes reference the mesh. Vertices and indices are shared; placement
// lives on the per-node SubMesh copies made from this template.
struct EmittedGeometry {
    VulkanEngine::SubMesh submesh{}; // transform deliberately left identity
    bool authored_tangents = false;  // the primitive carried TANGENT
    bool authored_uv0 = false;       // the primitive carried TEXCOORD_0
};

struct ConversionState {
    const fastgltf::Asset& asset;
    const std::vector<VulkanEngine::MaterialId>* bindings = nullptr;
    VulkanEngine::Mesh& mesh;
    // One entry per emitted primitive. The ranges are disjoint, which also
    // makes this the list tangent generation runs over: exactly once per range.
    std::vector<EmittedGeometry> geometry;
    // glTF mesh index -> the geometry entries it emitted, filled the first time
    // a node references the mesh.
    std::unordered_map<std::size_t, std::vector<std::size_t>> mesh_geometry;
    bool any_uv_authored = false;
    bool any_uv1 = false;
    bool warned_unsupported_mode = false;
    bool warned_extra_uv_set = false;
    bool warned_material = false;
    // JOINTS_4/WEIGHTS_4 or beyond: past the four sets that can contribute to a
    // vertex's eight influence slots.
    bool warned_extra_influence_set = false;
    bool warned_joint_range = false;
};

// glTF does not cap the number of JOINTS_n/WEIGHTS_n sets, but a vertex holds at
// most eight influences here, so four sets (16 candidates) is the widest input
// that can matter. Sets past the fourth are reported and ignored.
inline constexpr std::size_t kMaxInfluenceSets = 4;
// One GpuBoneInfluence record holds four influences, two records per vertex.
inline constexpr std::size_t kMaxVertexInfluences = 8;
// A dropped influence lighter than this is quantization noise that
// NormalizeAndQuantizeWeights would discard anyway, so dropping it does not by
// itself justify a "too many influences" warning.
inline constexpr float kInfluenceDropEpsilon = 1e-4f;

// Makes the per-vertex influence streams parallel. Once any primitive of the
// mesh is skinned, every vertex carries a BoneWeight (zeros for unskinned
// primitives), matching the existing uv_extra rule. Existing vertices are
// backfilled when the first skinned primitive or the first JOINTS_1 set appears.
void EnsureSkinStreams(ConversionState& state, const bool want_extra) {
    auto& mesh = state.mesh;
    if (mesh.influence_sets == 0) {
        mesh.influence_sets = 1;
        mesh.bone_weights.resize(mesh.vertices.size());
    }
    if (want_extra && mesh.influence_sets < 2) {
        mesh.influence_sets = 2;
        mesh.bone_weights.resize(mesh.vertices.size());
        mesh.bone_weights_extra.resize(mesh.vertices.size());
    }
}

// Appends one vertex and returns its mesh-local index. Every parallel array is
// kept parallel: a set that appears in any primitive is populated for all
// vertices (zeros where the primitive does not author it), because downstream
// consumers index uvs/tangents/weights by vertex.
[[nodiscard]] std::uint32_t EmitVertex(ConversionState& state,
                                       const MeshVertexVec3& position,
                                       const MeshVertexVec3& normal,
                                       const MeshVertexVec2& uv0,
                                       const MeshVertexVec2& uv1,
                                       const MeshVertexVec3& tangent,
                                       const float handedness,
                                       const BoneWeight& base_weights,
                                       const BoneWeight& extra_weights) {
    auto& mesh = state.mesh;
    const auto index = static_cast<std::uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(position);
    mesh.normals.push_back(normal);
    mesh.uvs.push_back(uv0);
    if (state.any_uv1) {
        mesh.uv_extra[0].push_back(uv1);
    }
    mesh.tangents.push_back(tangent);
    mesh.tangent_handedness.push_back(handedness);
    if (mesh.influence_sets > 0) {
        mesh.bone_weights.push_back(base_weights);
    }
    if (mesh.influence_sets >= 2) {
        mesh.bone_weights_extra.push_back(extra_weights);
    }
    return index;
}

// Expands a primitive's index list (or its implicit one) into triangles. The
// engine draws triangles only, so strips and fans are triangulated here.
[[nodiscard]] bool Triangulate(const fastgltf::PrimitiveType mode,
                               std::vector<std::uint32_t> source,
                               std::vector<std::uint32_t>& out) {
    const auto push = [&out](const std::uint32_t a, const std::uint32_t b, const std::uint32_t c) {
        out.push_back(a);
        out.push_back(b);
        out.push_back(c);
    };

    switch (mode) {
        case fastgltf::PrimitiveType::Triangles:
            out = std::move(source);
            return true;
        case fastgltf::PrimitiveType::TriangleStrip:
            for (std::size_t i = 0; i + 2 < source.size(); ++i) {
                if (i % 2 == 0) {
                    push(source[i], source[i + 1], source[i + 2]);
                } else {
                    push(source[i + 1], source[i], source[i + 2]);
                }
            }
            return true;
        case fastgltf::PrimitiveType::TriangleFan:
            for (std::size_t i = 1; i + 1 < source.size(); ++i) {
                push(source[0], source[i], source[i + 1]);
            }
            return true;
        default:
            return false;
    }
}

[[nodiscard]] MeshVertexVec3 FaceNormal(const MeshVertexVec3& a,
                                        const MeshVertexVec3& b,
                                        const MeshVertexVec3& c) {
    const MeshVertexVec3 e1{b.x - a.x, b.y - a.y, b.z - a.z};
    const MeshVertexVec3 e2{c.x - a.x, c.y - a.y, c.z - a.z};
    MeshVertexVec3 n{
        e1.y * e2.z - e1.z * e2.y,
        e1.z * e2.x - e1.x * e2.z,
        e1.x * e2.y - e1.y * e2.x,
    };
    const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (length > 1e-8f) {
        n.x /= length;
        n.y /= length;
        n.z /= length;
    } else {
        n = MeshVertexVec3{0.0f, 1.0f, 0.0f}; // degenerate triangle: a valid unit normal
    }
    return n;
}

// Emits one primitive's geometry into the mesh and returns its index into
// state.geometry, or nothing when the primitive has nothing to draw. The
// placement transform is deliberately not applied here: the same geometry is
// reused by every node that references the mesh, so it is stored unplaced.
// `gltf_mesh`/`primitive_index` only name the source in diagnostics.
[[nodiscard]] std::optional<std::size_t> ConvertPrimitive(ConversionState& state,
                                                          const fastgltf::Primitive& primitive,
                                                          const std::size_t gltf_mesh,
                                                          const std::size_t primitive_index) {
    auto& mesh = state.mesh;
    const auto& asset = state.asset;

    if (primitive.dracoCompression) {
        throw std::runtime_error(
            "GltfMeshAssembler: the asset uses KHR_draco_mesh_compression, which is not supported");
    }

    const auto position_it = primitive.findAttribute("POSITION");
    if (position_it == primitive.attributes.end()) {
        return std::nullopt; // a primitive with no positions has nothing to draw
    }

    const std::vector<fastgltf::math::fvec3> positions =
        ReadAccessor<fastgltf::math::fvec3>(asset, position_it->accessorIndex);
    if (positions.empty()) return std::nullopt;

    const auto normal_it = primitive.findAttribute("NORMAL");
    const std::vector<fastgltf::math::fvec3> normals = normal_it != primitive.attributes.end()
        ? ReadAccessor<fastgltf::math::fvec3>(asset, normal_it->accessorIndex)
        : std::vector<fastgltf::math::fvec3>{};

    const auto uv0_it = primitive.findAttribute("TEXCOORD_0");
    const std::vector<fastgltf::math::fvec2> uv0 = uv0_it != primitive.attributes.end()
        ? ReadAccessor<fastgltf::math::fvec2>(asset, uv0_it->accessorIndex)
        : std::vector<fastgltf::math::fvec2>{};

    const auto uv1_it = primitive.findAttribute("TEXCOORD_1");
    const std::vector<fastgltf::math::fvec2> uv1 = uv1_it != primitive.attributes.end()
        ? ReadAccessor<fastgltf::math::fvec2>(asset, uv1_it->accessorIndex)
        : std::vector<fastgltf::math::fvec2>{};

    const auto tangent_it = primitive.findAttribute("TANGENT");
    const std::vector<fastgltf::math::fvec4> tangents = tangent_it != primitive.attributes.end()
        ? ReadAccessor<fastgltf::math::fvec4>(asset, tangent_it->accessorIndex)
        : std::vector<fastgltf::math::fvec4>{};

    // JOINTS_n/WEIGHTS_n are read as 32-bit/float views. fastgltf converts the
    // accessor's own component type (u8/u16 joints, normalized u8/u16 or f32
    // weights), so no per-type branch is needed. The stored joint index stays
    // skin-local: Skin::joints maps skin joint k to the skeleton joint whose
    // palette entry the shader indexes.
    const auto read_influences = [&asset, &primitive](const char* joints_name,
                                                      const char* weights_name)
        -> std::pair<std::vector<fastgltf::math::u32vec4>, std::vector<fastgltf::math::fvec4>> {
        const auto joints_it = primitive.findAttribute(joints_name);
        const auto weights_it = primitive.findAttribute(weights_name);
        if (joints_it == primitive.attributes.end() || weights_it == primitive.attributes.end()) {
            return {};
        }
        if (joints_it->accessorIndex >= asset.accessors.size() ||
            weights_it->accessorIndex >= asset.accessors.size()) {
            return {};
        }
        if (asset.accessors[joints_it->accessorIndex].type != fastgltf::AccessorType::Vec4 ||
            asset.accessors[weights_it->accessorIndex].type != fastgltf::AccessorType::Vec4) {
            return {};
        }
        return {ReadAccessor<fastgltf::math::u32vec4>(asset, joints_it->accessorIndex),
                ReadAccessor<fastgltf::math::fvec4>(asset, weights_it->accessorIndex)};
    };
    auto [joints0, weights0] = read_influences("JOINTS_0", "WEIGHTS_0");
    const bool has_skin0 = weights0.size() >= positions.size() && joints0.size() >= positions.size();

    // Sets 1..3 are optional; a primitive may also author none of them while a
    // sibling primitive does (the streams are made parallel by EnsureSkinStreams
    // and EmitVertex, zeros where a primitive has no such set).
    struct InfluenceSet {
        std::vector<fastgltf::math::u32vec4> joints;
        std::vector<fastgltf::math::fvec4> weights;
    };
    std::array<InfluenceSet, kMaxInfluenceSets> sets{};
    if (has_skin0) {
        sets[0] = InfluenceSet{std::move(joints0), std::move(weights0)};
        for (std::size_t s = 1; s < kMaxInfluenceSets; ++s) {
            const std::string joints_name = "JOINTS_" + std::to_string(s);
            const std::string weights_name = "WEIGHTS_" + std::to_string(s);
            auto [joints, weights] = read_influences(joints_name.c_str(), weights_name.c_str());
            if (weights.size() >= positions.size() && joints.size() >= positions.size()) {
                sets[s] = InfluenceSet{std::move(joints), std::move(weights)};
            }
        }
    }
    bool has_extra_set = false;
    for (std::size_t s = 1; s < kMaxInfluenceSets; ++s) {
        has_extra_set = has_extra_set || !sets[s].weights.empty();
    }

    // Sets past the fourth cannot reach a vertex's eight slots, and pretending
    // otherwise would silently reshape strong influences. Report it once per mesh.
    if (!state.warned_extra_influence_set &&
        (primitive.findAttribute("JOINTS_4") != primitive.attributes.end() ||
         primitive.findAttribute("WEIGHTS_4") != primitive.attributes.end())) {
        state.warned_extra_influence_set = true;
        LOGIFACE_LOG(warn, "GltfMeshAssembler: mesh " + std::to_string(gltf_mesh) + " primitive " +
                               std::to_string(primitive_index) +
                               " declares JOINTS_4/WEIGHTS_4 or beyond; a vertex holds at most " +
                               std::to_string(kMaxVertexInfluences) +
                               " influences, so only the first four sets are read");
    }
    if (has_skin0) EnsureSkinStreams(state, has_extra_set);

    // Reports, once per primitive, that some vertex carried more than eight
    // non-trivial influences and had to drop the smallest ones. This is the
    // documented limitation rather than a silent approximation: an asset that
    // splits 9+ influences across sets keeps its heaviest eight per vertex, not
    // the first eight by set order.
    bool dropped_influences = false;

    // Converts one corner's glTF influences into the two engine streams. Every
    // candidate influence from every set is collected and the heaviest eight are
    // kept, so a mesh whose strong weights live in the second or third set is not
    // truncated by set order. Weights are then normalized and quantized across
    // all eight, so the two records together sum to exactly 255; the stored joint
    // index stays skin-local.
    const auto build_weights = [&](const std::uint32_t index) {
        BoneWeight base{};
        BoneWeight extra{};
        if (!has_skin0) return std::pair{base, extra};

        struct Candidate {
            float weight;
            std::uint32_t joint;
        };
        std::array<Candidate, kMaxInfluenceSets * 4> candidates{};
        std::size_t count = 0;
        for (const InfluenceSet& set : sets) {
            if (set.weights.empty()) continue;
            for (std::size_t k = 0; k < 4; ++k) {
                const float weight = set.weights[index][k];
                if (weight > 0.0f) {
                    candidates[count++] = Candidate{weight, set.joints[index][k]};
                }
            }
        }
        if (count == 0) return std::pair{base, extra};

        if (count > kMaxVertexInfluences) {
            // Stable, so a mesh with tied weights keeps a deterministic order (the
            // dedup key below includes the eight slots, so an unstable order would
            // make vertex merging depend on luck).
            std::stable_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(count),
                             [](const Candidate& a, const Candidate& b) {
                                 return a.weight > b.weight;
                             });
            if (candidates[kMaxVertexInfluences].weight > kInfluenceDropEpsilon) {
                dropped_influences = true;
            }
            count = kMaxVertexInfluences;
        }

        std::array<float, 8> weights{};
        std::array<std::uint32_t, 8> joints{};
        for (std::size_t k = 0; k < count; ++k) {
            weights[k] = candidates[k].weight;
            joints[k] = candidates[k].joint;
        }
        if (!NormalizeAndQuantizeWeights(weights)) return std::pair{base, extra};
        const auto quantize = [](const float weight) {
            return static_cast<std::uint8_t>(
                std::lround(std::clamp(weight, 0.0f, 1.0f) * 255.0f));
        };
        const auto to_joint = [&state](const std::uint32_t joint) {
            if (joint > std::numeric_limits<std::uint16_t>::max()) {
                if (!state.warned_joint_range) {
                    state.warned_joint_range = true;
                    LOGIFACE_LOG(warn, "GltfMeshAssembler: a JOINTS attribute stores an index above "
                                       "65535; the influence record is 16-bit, so the index is "
                                       "clamped to 0 to keep the palette read in range");
                }
                return std::uint16_t{0};
            }
            return static_cast<std::uint16_t>(joint);
        };
        for (std::size_t k = 0; k < 4; ++k) {
            base.weights[k] = quantize(weights[k]);
            extra.weights[k] = quantize(weights[4 + k]);
            base.bone_indices[k] = to_joint(joints[k]);
            extra.bone_indices[k] = to_joint(joints[4 + k]);
        }
        return std::pair{base, extra};
    };

    // Sets 2 and 3 have no home while MAX_UV_SETS is 2. The shader resolves a
    // material's absent set to UV0, so dropping them degrades instead of breaks.
    if (!state.warned_extra_uv_set &&
        (primitive.findAttribute("TEXCOORD_2") != primitive.attributes.end() ||
         primitive.findAttribute("TEXCOORD_3") != primitive.attributes.end())) {
        state.warned_extra_uv_set = true;
        LOGIFACE_LOG(warn, "GltfMeshAssembler: TEXCOORD_2/3 present but MAX_UV_SETS is 2; the extra UV set(s) are ignored");
    }

    std::vector<std::uint32_t> source_indices;
    if (primitive.indicesAccessor) {
        source_indices = ReadAccessor<std::uint32_t>(asset, *primitive.indicesAccessor);
    } else {
        source_indices.resize(positions.size());
        std::iota(source_indices.begin(), source_indices.end(), 0u);
    }

    std::vector<std::uint32_t> triangles;
    if (!Triangulate(primitive.type, std::move(source_indices), triangles)) {
        if (!state.warned_unsupported_mode) {
            state.warned_unsupported_mode = true;
            LOGIFACE_LOG(warn, "GltfMeshAssembler: skipping a non-triangle primitive "
                               "(only TRIANGLES, TRIANGLE_STRIP and TRIANGLE_FAN are supported)");
        }
        return std::nullopt;
    }
    triangles.resize(triangles.size() - (triangles.size() % 3U));

    VulkanEngine::MaterialId material_id{0};
    if (primitive.materialIndex) {
        material_id = MapGltfMaterialIndex(*primitive.materialIndex, state.bindings);
        if (!state.warned_material &&
            (state.bindings == nullptr || *primitive.materialIndex >= state.bindings->size())) {
            state.warned_material = true;
            LOGIFACE_LOG(warn, "GltfMeshAssembler: material index " + std::to_string(*primitive.materialIndex) +
                               " has no binding; the default material is used");
        }
    }

    const bool has_normals = normals.size() >= positions.size();
    const bool has_tangents = tangents.size() >= positions.size();
    const bool has_uv0 = uv0.size() >= positions.size();
    const bool has_uv1 = uv1.size() >= positions.size();
    if (has_uv0) state.any_uv_authored = true;

    // A UV set that appears in any primitive must be parallel for every vertex.
    if (has_uv1 && !state.any_uv1) {
        state.any_uv1 = true;
        mesh.uv_extra[0].resize(mesh.vertices.size(), MeshVertexVec2{0.0f, 0.0f});
    }

    const std::uint32_t index_start = static_cast<std::uint32_t>(mesh.indices.size());

    if (has_normals) {
        std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> dedup;
        dedup.reserve(triangles.size());
        for (const std::uint32_t index : triangles) {
            if (index >= positions.size()) continue; // malformed asset: drop the corner
            const MeshVertexVec3 position = ToVec3(positions[index]);
            const MeshVertexVec3 normal = ToVec3(normals[index]);
            const MeshVertexVec2 tex0 = has_uv0 ? ToVec2(uv0[index]) : MeshVertexVec2{0.0f, 0.0f};
            const MeshVertexVec2 tex1 = has_uv1 ? ToVec2(uv1[index]) : MeshVertexVec2{0.0f, 0.0f};
            MeshVertexVec3 tangent{0.0f, 0.0f, 0.0f};
            float handedness = 1.0f;
            if (has_tangents) {
                tangent = MeshVertexVec3{tangents[index].x(), tangents[index].y(), tangents[index].z()};
                handedness = tangents[index].w();
            }
            const float tangent4[4] = {tangent.x, tangent.y, tangent.z, handedness};
            const auto [base_weights, extra_weights] = build_weights(index);
            std::array<std::uint32_t, 8> key_joints{};
            std::array<std::uint8_t, 8> key_weights{};
            for (std::size_t k = 0; k < 4; ++k) {
                key_joints[k] = base_weights.bone_indices[k];
                key_weights[k] = base_weights.weights[k];
                key_joints[4 + k] = extra_weights.bone_indices[k];
                key_weights[4 + k] = extra_weights.weights[k];
            }

            const auto [it, inserted] = dedup.try_emplace(
                MakeKey(position, normal, tex0, tex1, tangent4, key_joints, key_weights, has_tangents), 0u);
            if (inserted) {
                it->second = EmitVertex(state, position, normal, tex0, tex1, tangent, handedness,
                                        base_weights, extra_weights);
            }
            mesh.indices.push_back(it->second);
        }
    } else {
        // No NORMAL: generate flat per-triangle normals, which the spec allows a
        // consumer to do and which keeps the octahedral packing well defined.
        // The corners are deliberately not deduplicated: adjacent triangles
        // share an edge but not a normal.
        for (std::size_t t = 0; t + 2 < triangles.size(); t += 3) {
            const std::uint32_t i0 = triangles[t];
            const std::uint32_t i1 = triangles[t + 1];
            const std::uint32_t i2 = triangles[t + 2];
            if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size()) continue;
            const MeshVertexVec3 p0 = ToVec3(positions[i0]);
            const MeshVertexVec3 p1 = ToVec3(positions[i1]);
            const MeshVertexVec3 p2 = ToVec3(positions[i2]);
            const MeshVertexVec3 normal = FaceNormal(p0, p1, p2);
            const std::uint32_t corners[3] = {i0, i1, i2};
            for (const std::uint32_t corner : corners) {
                const MeshVertexVec2 tex0 = has_uv0 ? ToVec2(uv0[corner]) : MeshVertexVec2{0.0f, 0.0f};
                const MeshVertexVec2 tex1 = has_uv1 ? ToVec2(uv1[corner]) : MeshVertexVec2{0.0f, 0.0f};
                // An authored tangent is honored here too; only the normal is
                // synthesized, so the file's tangent space is not discarded.
                MeshVertexVec3 tangent{0.0f, 0.0f, 0.0f};
                float handedness = 1.0f;
                if (has_tangents) {
                    tangent = MeshVertexVec3{tangents[corner].x(), tangents[corner].y(), tangents[corner].z()};
                    handedness = tangents[corner].w();
                }
                const auto [base_weights, extra_weights] = build_weights(corner);
                mesh.indices.push_back(
                    EmitVertex(state, ToVec3(positions[corner]), normal, tex0, tex1, tangent, handedness,
                               base_weights, extra_weights));
            }
        }
    }

    if (dropped_influences) {
        LOGIFACE_LOG(warn, "GltfMeshAssembler: mesh " + std::to_string(gltf_mesh) + " primitive " +
                               std::to_string(primitive_index) + " authors more than " +
                               std::to_string(kMaxVertexInfluences) +
                               " influences per vertex; the smallest weights are dropped and the "
                               "remaining ones renormalized");
    }

    const std::uint32_t index_count = static_cast<std::uint32_t>(mesh.indices.size()) - index_start;
    if (index_count == 0) return std::nullopt;

    EmittedGeometry emitted{};
    emitted.submesh.index_start = index_start;
    emitted.submesh.index_count = index_count;
    emitted.submesh.material_id = material_id;
    emitted.authored_tangents = has_tangents;
    emitted.authored_uv0 = has_uv0;
    state.geometry.push_back(emitted);
    return state.geometry.size() - 1U;
}

// Fills the tangent frame: the file's baked TANGENT where a primitive authored
// one, MikkTSpace for the rest. Runs after all geometry exists, because
// MikkTSpace accumulates across shared vertices and must see the deduplicated
// geometry, and once per emitted primitive rather than once per placed submesh.
//
// No part of this discards file data. A primitive that cannot be generated from
// — no UV domain, or a generator failure — keeps the zero tangent that already
// means "no tangent space", and the tangents authored elsewhere in the mesh
// survive it.
void FinalizeTangents(ConversionState& state) {
    auto& mesh = state.mesh;
    if (mesh.vertices.empty()) return;

    // EmitVertex stored a zero tangent and unit handedness per vertex already;
    // hold that shape so a mesh can carry authored tangents beside generated
    // ones.
    mesh.tangents.resize(mesh.vertices.size(), MeshVertexVec3{0.0f, 0.0f, 0.0f});
    mesh.tangent_handedness.resize(mesh.vertices.size(), 1.0f);

    bool any_authored = false;
    for (const EmittedGeometry& emitted : state.geometry) {
        any_authored |= emitted.authored_tangents;
    }

    if (state.any_uv_authored) {
        // One pass per emitted range. Node copies share their geometry, so
        // running per submesh would regenerate identical tangents per placement.
        for (const EmittedGeometry& emitted : state.geometry) {
            if (emitted.authored_tangents) continue;
            // A primitive with no TEXCOORD_0 carries fabricated zero UVs; there
            // is no tangent space to derive from them, so leave it at zero
            // rather than feeding MikkTSpace a degenerate domain.
            if (!emitted.authored_uv0) continue;
            const SubMesh& submesh = emitted.submesh;
            if (VulkanEngine::TangentGen::GenerateSubmeshTangents(
                    mesh.vertices, mesh.normals, mesh.uvs, mesh.indices,
                    submesh.index_start, submesh.index_count,
                    mesh.tangents, mesh.tangent_handedness)) {
                continue;
            }
            // The generator rejected this submesh. Only the values it would have
            // derived are lost: its corners keep the zero tangent, and a bad
            // submesh no longer costs the whole mesh its tangent frame.
            LOGIFACE_LOG(warn, "GltfMeshAssembler: MikkTSpace rejected a submesh; its "
                               "generated tangents are left at zero");
            for (std::uint32_t i = 0; i < submesh.index_count; ++i) {
                const std::uint32_t vertex = mesh.indices[submesh.index_start + i];
                mesh.tangents[vertex] = MeshVertexVec3{0.0f, 0.0f, 0.0f};
                mesh.tangent_handedness[vertex] = 1.0f;
            }
        }
    }

    // An all-zero stream says nothing the empty one does not; drop it so the
    // "no tangent space" representation stays the single one the engine reads.
    const bool any_tangent = any_authored ||
        std::ranges::any_of(mesh.tangents, [](const MeshVertexVec3& t) {
            return t.x != 0.0f || t.y != 0.0f || t.z != 0.0f;
        });
    if (!any_tangent) {
        mesh.tangents.clear();
        mesh.tangent_handedness.clear();
    }
}

void ConvertAsset(const fastgltf::Asset& asset,
                  const std::vector<VulkanEngine::MaterialId>* bindings,
                  VulkanEngine::Mesh& mesh) {
    ConversionState state{asset, bindings, mesh};

    // Skeletons and skins are independent of the node traversal: they are built
    // from asset.skins and the node hierarchy first, so a skinned placement can
    // resolve its skin_index and the animation pass can map channel targets.
    const std::vector<std::size_t> node_parents = BuildParentMap(asset);
    NodeWorldCache node_world(asset, node_parents);
    const SkinImport skin_import = BuildSkeletonsAndSkins(asset, node_parents, node_world, mesh);

    // Which nodes are reachable as roots: the default scene when it names one,
    // else scene 0, else every node that is nobody's child. An asset with no
    // scenes (legal) still imports.
    std::vector<bool> is_child(asset.nodes.size(), false);
    for (const auto& node : asset.nodes) {
        for (const std::size_t child : node.children) {
            if (child < is_child.size()) is_child[child] = true;
        }
    }

    std::vector<std::size_t> roots;
    const auto append_scene_roots = [&roots, &asset](const std::size_t scene_index) {
        if (scene_index >= asset.scenes.size()) return;
        for (const std::size_t node : asset.scenes[scene_index].nodeIndices) roots.push_back(node);
    };
    if (asset.defaultScene.has_value()) append_scene_roots(*asset.defaultScene);
    if (roots.empty() && !asset.scenes.empty()) append_scene_roots(0);
    if (roots.empty()) {
        for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
            if (!is_child[i]) roots.push_back(i);
        }
    }

    // Each stack entry carries the transform of the node's *parent*; the node's
    // own local transform is composed when it is visited, so a root's own
    // transform is applied exactly once.
    struct Pending {
        std::size_t node = 0;
        SubMeshTransform parent_trs{};
        fastgltf::math::fmat4x4 parent_matrix{};
    };

    std::vector<Pending> stack;
    stack.reserve(roots.size());
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
        stack.push_back(Pending{*it, SubMeshTransform{}, fastgltf::math::fmat4x4{}});
    }

    std::unordered_set<std::size_t> shear_warned;
    while (!stack.empty()) {
        const Pending parent = stack.back();
        stack.pop_back();
        if (parent.node >= asset.nodes.size()) continue;
        const fastgltf::Node& node = asset.nodes[parent.node];

        const SubMeshTransform node_trs = ComposeSubmeshTransform(parent.parent_trs, LocalTrs(node));
        const fastgltf::math::fmat4x4 node_matrix = parent.parent_matrix * LocalMatrix(node);

        if (HasShear(node_matrix) && shear_warned.insert(parent.node).second) {
            LOGIFACE_LOG(warn, "GltfMeshAssembler: node " + std::to_string(parent.node) +
                               " accumulates a sheared transform; the engine's TRS placement cannot "
                               "represent it, so this node is placed approximately");
        }

        // A skinned node's own transform is ignored by glTF: the skeleton drives
        // the vertices. The placement therefore stays identity and the object/
        // skeleton root transform is applied through the entity Transform or the
        // SkeletonRoot, never through the submesh placement.
        std::optional<std::uint32_t> placement_skin;
        if (node.skinIndex.has_value()) {
            const std::size_t skin_index = *node.skinIndex;
            // `mesh.skins` is sized from asset.skins, so a skipped skin (invalid or
            // oversized joint list) is in range but has no skeleton. Requiring a
            // resolved skeleton keeps that malformed asset on the unskinned path,
            // with its node transform, instead of identity-placing geometry whose
            // skin has no palette.
            if (skin_index < mesh.skins.size() &&
                mesh.skins[skin_index].skeleton != kNoSkeleton &&
                !mesh.skins[skin_index].joints.empty()) {
                placement_skin = static_cast<std::uint32_t>(skin_index);
            } else {
                LOGIFACE_LOG(warn, "GltfMeshAssembler: node " + std::to_string(parent.node) +
                                       " references skin " + std::to_string(skin_index) +
                                       ", which is missing or was not imported; the node is placed "
                                       "unskinned");
            }
        }

        // KHR_physics_rigid_bodies lives on the node, so every submesh placed
        // for this node carries the same hint — including several primitives of
        // one mesh, which are one body in the extension's model.
        const std::optional<VulkanEngine::PhysicsHint> node_physics =
            BuildPhysicsHint(asset, node);
        if (node_physics.has_value() && !node.meshIndex.has_value()) {
            LOGIFACE_LOG(debug, "GltfMeshAssembler: node " + std::to_string(parent.node) +
                                    " declares KHR_physics_rigid_bodies but has no mesh; the collider "
                                    "hint has no submesh to attach to and is dropped");
        }

        if (node.meshIndex.has_value() && *node.meshIndex < asset.meshes.size()) {
            const std::size_t gltf_mesh = *node.meshIndex;
            // glTF instances a mesh by referencing it from several nodes. The
            // geometry is emitted once and then placed once per reference, so a
            // mesh shared by eight pawns costs one copy, not eight. The same
            // holds for instances with different skins: the vertex stream is
            // shared because stored joint indices are skin-local.
            auto cached = state.mesh_geometry.find(gltf_mesh);
            if (cached == state.mesh_geometry.end()) {
                std::vector<std::size_t> emitted;
                const auto& primitives = asset.meshes[gltf_mesh].primitives;
                for (std::size_t primitive_index = 0; primitive_index < primitives.size();
                     ++primitive_index) {
                    if (const auto index =
                            ConvertPrimitive(state, primitives[primitive_index], gltf_mesh, primitive_index)) {
                        emitted.push_back(*index);
                    }
                }
                cached = state.mesh_geometry.emplace(gltf_mesh, std::move(emitted)).first;
            }
            for (const std::size_t index : cached->second) {
                VulkanEngine::SubMesh placed = state.geometry[index].submesh;
                placed.transform = placement_skin.has_value() ? SubMeshTransform{} : node_trs;
                placed.physics = node_physics;
                placed.skin_index = placement_skin.value_or(kNoSkin);
                mesh.subMeshes.push_back(placed);
            }
        }

        for (const std::size_t child : node.children) {
            if (child >= asset.nodes.size()) continue;
            stack.push_back(Pending{child, node_trs, node_matrix});
        }
    }

    BuildAnimations(asset, skin_import, mesh);
    FinalizeTangents(state);
}

} // namespace

std::future<std::shared_ptr<VulkanEngine::Mesh>>
GltfMeshAssembler::AssembleFromFullBuffer(std::shared_ptr<FileLoader::ByteBuffer> buffer) {
    auto prom = std::make_shared<std::promise<std::shared_ptr<VulkanEngine::Mesh>>>();
    try {
        if (source_path_.empty()) {
            throw std::runtime_error(
                "GltfMeshAssembler: SetSourcePath must be called before loading: external buffers and "
                "images resolve against the asset's own directory");
        }
        if (buffer == nullptr || buffer->empty()) {
            throw std::runtime_error("GltfMeshAssembler: empty buffer for '" + source_path_.string() + "'");
        }

        // Extensions the asset may use. Quantization is a geometry extension, so
        // it belongs here rather than only in the material importer.
        // KHR_physics_rigid_bodies (and the KHR_implicit_shapes shapes it
        // references) are asked for explicitly: without the bit fastgltf skips
        // the node extension object entirely, and the collider hints would
        // silently vanish.
        fastgltf::Parser parser(fastgltf::Extensions::KHR_mesh_quantization |
                                fastgltf::Extensions::KHR_texture_transform |
                                fastgltf::Extensions::KHR_texture_basisu |
                                fastgltf::Extensions::KHR_materials_emissive_strength |
                                fastgltf::Extensions::KHR_materials_unlit |
                                fastgltf::Extensions::KHR_physics_rigid_bodies |
                                fastgltf::Extensions::KHR_implicit_shapes);

        auto data = fastgltf::GltfDataBuffer::FromBytes(
            reinterpret_cast<const std::byte*>(buffer->data()), buffer->size());
        if (!data) {
            throw std::runtime_error("GltfMeshAssembler: could not create a GLTF data buffer");
        }

        // loadGltf detects JSON vs GLB, so .gltf and .glb share one path;
        // external buffers resolve against the asset's directory. Images are
        // deliberately left alone: this loader never reads asset.images, and
        // asking fastgltf to load them would make a missing or unreadable
        // texture fail a mesh load (reported as "An external buffer was not
        // found", which names the wrong resource) after reading every image
        // once here and again in ImportGltfMaterials. A `data:` URI is decoded
        // by fastgltf whether or not this option is set.
        auto asset = parser.loadGltf(data.get(), source_path_.parent_path(),
                                     fastgltf::Options::LoadExternalBuffers);
        if (asset.error() != fastgltf::Error::None) {
            throw std::runtime_error("GltfMeshAssembler: failed to load '" + source_path_.string() +
                                     "': " + std::string(fastgltf::getErrorMessage(asset.error())));
        }

        auto mesh = std::make_shared<VulkanEngine::Mesh>();
        ConvertAsset(asset.get(), material_bindings_, *mesh);

        if (mesh->vertices.empty()) {
            // A "successful" parse that yields nothing is worse than a failure:
            // the caller would draw an empty mesh with no indication why.
            throw std::runtime_error("GltfMeshAssembler: no renderable geometry in '" + source_path_.string() + "'");
        }

        prom->set_value(mesh);
    } catch (...) {
        prom->set_exception(std::current_exception());
    }
    return prom->get_future();
}

std::shared_ptr<VulkanEngine::Mesh> GltfMeshLoader::DoLoad(const std::filesystem::path& path) {
    auto buf = ReadEntireFile(path);
    auto buf_ptr = std::make_shared<FileLoader::ByteBuffer>(buf.begin(), buf.end());
    GltfMeshAssembler assembler;
    assembler.SetMaterialBindings(material_bindings_);
    assembler.SetSourcePath(path);
    return assembler.AssembleFromFullBuffer(std::move(buf_ptr)).get();
}

std::vector<std::byte> GltfMeshLoader::ReadEntireFile(const std::filesystem::path& file_path) {
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("GltfMeshLoader: Failed to open file: " + file_path.string());
    }
    const auto size = static_cast<std::size_t>(file.tellg());
    if (size == 0) {
        throw std::runtime_error("GltfMeshLoader: File is empty: " + file_path.string());
    }
    std::vector<std::byte> buf(size);
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size))) {
        throw std::runtime_error("GltfMeshLoader: Failed to read file: " + file_path.string());
    }
    return buf;
}

} // namespace VulkanEngine::FileLoaders::Mesh
