module;

#include <logging/logging_macros.hpp>

module VulkanEngine.Mesh.SkeletalAnimation;

import std;

import logiface;

import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.GpuResources.MeshData;

namespace VulkanEngine {

namespace {

using Mat4 = std::array<float, 16>;

// Weights below this fraction of the total are treated as noise and dropped
// before quantization. They cannot influence a vertex visibly, and dropping
// them keeps a stray tiny influence from consuming a joint slot.
constexpr float kWeightEpsilon = 1e-4f;

[[nodiscard]] constexpr Mat4 IdentityMatrix() noexcept {
    return Mat4{1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f};
}

// Column-major a * b.
[[nodiscard]] Mat4 Multiply(const Mat4& a, const Mat4& b) noexcept {
    Mat4 out{};
    for (std::size_t c = 0; c < 4; ++c) {
        for (std::size_t r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (std::size_t k = 0; k < 4; ++k) {
                sum += a[k * 4 + r] * b[c * 4 + k];
            }
            out[c * 4 + r] = sum;
        }
    }
    return out;
}

[[nodiscard]] std::array<float, 4> NormalizeQuaternion(std::array<float, 4> q) noexcept {
    const float length2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (length2 <= 1e-12f) return {0.0f, 0.0f, 0.0f, 1.0f};
    const float inv_length = 1.0f / std::sqrt(length2);
    for (float& component : q) component *= inv_length;
    return q;
}

// Shortest-arc spherical interpolation. Both endpoints are normalized and the
// second is negated on a negative dot product, so a quaternion pair that
// straddles the double cover still takes the short path.
[[nodiscard]] std::array<float, 4> Slerp(std::array<float, 4> a,
                                         std::array<float, 4> b,
                                         const float t) noexcept {
    a = NormalizeQuaternion(a);
    b = NormalizeQuaternion(b);
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.0f) {
        for (float& component : b) component = -component;
        dot = -dot;
    }
    if (dot > 0.9995f) {
        // Nearly parallel: nlerp is numerically safer than dividing by sin(theta).
        std::array<float, 4> out{};
        for (std::size_t i = 0; i < 4; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
        return NormalizeQuaternion(out);
    }
    const float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
    const float sin_theta = std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) / sin_theta;
    const float wb = std::sin(t * theta) / sin_theta;
    std::array<float, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) out[i] = a[i] * wa + b[i] * wb;
    return NormalizeQuaternion(out);
}

// Samples one channel at `time` (already wrapped). Empty channels leave the
// bind-pose value alone.
[[nodiscard]] std::array<float, 4> SampleChannelAt(const AnimationChannel& channel, const float time) {
    const auto& keys = channel.keys;
    const std::size_t last = keys.size() - 1U;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;

    // keys[i].time <= time < keys[i + 1].time.
    const auto upper = std::upper_bound(
        keys.begin(), keys.end(), time,
        [](const float value, const AnimationKey& key) { return value < key.time; });
    const std::size_t next = static_cast<std::size_t>(std::distance(keys.begin(), upper));
    const std::size_t prev = next - 1U;
    if (next > last) return keys.back().value;

    const AnimationKey& a = keys[prev];
    const AnimationKey& b = keys[next];
    if (channel.interpolation == AnimationInterpolation::Step) return a.value;

    const float span = b.time - a.time;
    const float f = (span > 0.0f) ? (time - a.time) / span : 0.0f;
    if (channel.path == AnimationPath::Rotation) {
        return Slerp(a.value, b.value, f);
    }
    std::array<float, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) out[i] = a.value[i] + (b.value[i] - a.value[i]) * f;
    return out;
}

} // namespace

Mat4 FromTRS(const SubMeshTransform& trs) {
    std::array<float, 4> q = NormalizeQuaternion({trs.rotation[0], trs.rotation[1], trs.rotation[2], trs.rotation[3]});
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;

    Mat4 out = IdentityMatrix();
    // Column 0 = R * (sx, 0, 0), and so on: the scale is applied in local space.
    out[0] = (1.0f - 2.0f * (yy + zz)) * trs.scale.x;
    out[1] = (2.0f * (xy + wz)) * trs.scale.x;
    out[2] = (2.0f * (xz - wy)) * trs.scale.x;

    out[4] = (2.0f * (xy - wz)) * trs.scale.y;
    out[5] = (1.0f - 2.0f * (xx + zz)) * trs.scale.y;
    out[6] = (2.0f * (yz + wx)) * trs.scale.y;

    out[8] = (2.0f * (xz + wy)) * trs.scale.z;
    out[9] = (2.0f * (yz - wx)) * trs.scale.z;
    out[10] = (1.0f - 2.0f * (xx + yy)) * trs.scale.z;

    out[12] = trs.position.x;
    out[13] = trs.position.y;
    out[14] = trs.position.z;
    return out;
}

void SampleClip(const AnimationClip& clip,
                const Skeleton& skeleton,
                const float time,
                const std::span<SubMeshTransform> out_local) {
    const std::size_t joint_count = out_local.size();
    for (std::size_t i = 0; i < joint_count && i < skeleton.joints.size(); ++i) {
        out_local[i] = skeleton.joints[i].local_bind;
    }

    float wrapped = time;
    if (clip.duration > 0.0f) {
        wrapped = std::fmod(time, clip.duration);
        if (wrapped < 0.0f) wrapped += clip.duration;
    } else {
        wrapped = std::max(time, 0.0f);
    }

    for (const AnimationChannel& channel : clip.channels) {
        if (channel.joint >= joint_count || channel.keys.empty()) continue;
        const std::array<float, 4> value = SampleChannelAt(channel, wrapped);
        SubMeshTransform& local = out_local[channel.joint];
        switch (channel.path) {
            case AnimationPath::Translation:
                local.position = MeshVertexVec3{value[0], value[1], value[2]};
                break;
            case AnimationPath::Rotation:
                local.rotation[0] = value[0];
                local.rotation[1] = value[1];
                local.rotation[2] = value[2];
                local.rotation[3] = value[3];
                break;
            case AnimationPath::Scale:
                local.scale = MeshVertexVec3{value[0], value[1], value[2]};
                break;
        }
    }
}

void ComputeWorld(const Skeleton& skeleton,
                  const std::span<const SubMeshTransform> local,
                  const std::span<std::array<float, 16>> out_world) {
    const std::size_t joint_count = std::min(skeleton.joints.size(), out_world.size());
    for (std::size_t i = 0; i < out_world.size(); ++i) out_world[i] = IdentityMatrix();
    if (joint_count == 0) return;
    if (local.empty()) {
        static bool warned_missing_local = false;
        if (!warned_missing_local) {
            warned_missing_local = true;
            LOGIFACE_LOG(warn, "SkeletalAnimation: ComputeWorld was given no local transforms; "
                               "the pose is left at identity");
        }
        return;
    }

    // Children are derived from parent indices, never from array order, so a
    // skeleton stored child-before-parent still resolves.
    std::vector<std::vector<std::uint32_t>> children(joint_count);
    for (std::size_t j = 0; j < joint_count; ++j) {
        const std::uint32_t parent = skeleton.joints[j].parent;
        if (parent != kNoSkeleton && parent < joint_count) {
            children[parent].push_back(static_cast<std::uint32_t>(j));
        }
    }

    std::vector<bool> visited(joint_count, false);
    std::vector<std::uint32_t> stack;
    stack.reserve(joint_count);
    for (const SkeletonRoot& root : skeleton.roots) {
        if (root.joint >= joint_count || visited[root.joint]) continue;
        const std::size_t local_index = std::min<std::size_t>(root.joint, local.size() - 1U);
        out_world[root.joint] = Multiply(root.transform, FromTRS(local[local_index]));
        visited[root.joint] = true;
        stack.push_back(root.joint);
        while (!stack.empty()) {
            const std::uint32_t parent = stack.back();
            stack.pop_back();
            for (const std::uint32_t child : children[parent]) {
                if (visited[child]) continue;
                const std::size_t child_local = std::min<std::size_t>(child, local.size() - 1U);
                out_world[child] = Multiply(out_world[parent], FromTRS(local[child_local]));
                visited[child] = true;
                stack.push_back(child);
            }
        }
    }

    if (std::ranges::count(visited, true) != static_cast<std::ptrdiff_t>(joint_count)) {
        // A malformed skeleton: report once, not once per frame.
        static bool warned_unreachable = false;
        if (!warned_unreachable) {
            warned_unreachable = true;
            LOGIFACE_LOG(warn, "SkeletalAnimation: a skeleton joint is not reachable from any "
                               "SkeletonRoot; its world transform is left at identity");
        }
    }
}

void ComputePalette(const Skin& skin,
                    const std::span<const std::array<float, 16>> world,
                    const std::span<std::array<float, 16>> out_palette) {
    for (std::size_t k = 0; k < out_palette.size(); ++k) {
        if (k >= skin.joints.size() || k >= skin.inverse_bind.size()) {
            out_palette[k] = IdentityMatrix();
            continue;
        }
        const std::uint32_t joint = skin.joints[k];
        if (joint >= world.size()) {
            out_palette[k] = IdentityMatrix();
            continue;
        }
        // Column-major M; the shader's row-vector multiply consumes these bytes
        // unchanged. See the module comment for the full convention.
        out_palette[k] = Multiply(world[joint], skin.inverse_bind[k]);
    }
}

void ResolveBindPose(const Skeleton& skeleton,
                     const std::span<std::array<float, 16>> out_world) {
    std::vector<SubMeshTransform> local(skeleton.joints.size());
    for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
        local[i] = skeleton.joints[i].local_bind;
    }
    ComputeWorld(skeleton, local, out_world);
}

bool NormalizeAndQuantizeWeights(std::array<float, 8>& in_out) {
    float sum = 0.0f;
    for (float& weight : in_out) {
        // `!(weight > epsilon)` drops NaN as well as tiny and negative values.
        if (!(weight > kWeightEpsilon)) {
            weight = 0.0f;
        } else {
            sum += weight;
        }
    }
    if (!(sum > 0.0f)) {
        in_out.fill(0.0f);
        return false;
    }

    // Quantize across all eight influences, then hand the rounding remainder to
    // the largest fractional parts so the integer weights sum to exactly 255.
    std::array<long long, 8> quantized{};
    std::array<float, 8> fraction{};
    long long total = 0;
    for (std::size_t i = 0; i < in_out.size(); ++i) {
        const float scaled = (in_out[i] / sum) * 255.0f;
        quantized[i] = static_cast<long long>(std::floor(scaled));
        fraction[i] = scaled - static_cast<float>(quantized[i]);
        total += quantized[i];
    }
    long long remainder = std::clamp(255LL - total, 0LL, 8LL);
    std::array<std::size_t, 8> order{};
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::ranges::sort(order, [&fraction](const std::size_t a, const std::size_t b) {
        return fraction[a] > fraction[b];
    });
    for (std::size_t i = 0; i < static_cast<std::size_t>(remainder); ++i) {
        ++quantized[order[i]];
    }
    for (std::size_t i = 0; i < in_out.size(); ++i) {
        in_out[i] = static_cast<float>(quantized[i]) / 255.0f;
    }
    return true;
}

std::shared_ptr<const SkeletalAsset> MakeSkeletalAsset(const GpuResources::MeshData& data) {
    auto asset = std::make_shared<SkeletalAsset>();
    asset->skeletons = data.skeletons;
    asset->skins = data.skins;
    asset->animations = data.animations;
    return asset;
}

} // namespace VulkanEngine
