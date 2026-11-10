module;

module VulkanEngine.Components.SkeletonPose;

import std;

import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.Mesh.SkeletalAnimation;

namespace VulkanEngine::Components {

namespace {

[[nodiscard]] constexpr std::array<float, 16> IdentityMatrix() noexcept {
    return {1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
}

} // namespace

void SkeletonPose::Rebuild() {
    local.clear();
    world.clear();
    palettes.clear();

    if (!asset || skeleton_index >= asset->skeletons.size()) {
        dirty = false;
        ++version;
        return;
    }

    const Skeleton& skeleton = asset->skeletons[skeleton_index];
    local.resize(skeleton.joints.size());
    world.assign(skeleton.joints.size(), IdentityMatrix());
    for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
        local[i] = skeleton.joints[i].local_bind;
    }
    ComputeWorld(skeleton, local, world);

    palettes.resize(asset->skins.size());
    for (std::size_t s = 0; s < asset->skins.size(); ++s) {
        const Skin& skin = asset->skins[s];
        if (skin.skeleton != skeleton_index) continue;
        palettes[s].assign(skin.joints.size(), IdentityMatrix());
        ComputePalette(skin, world, palettes[s]);
    }

    dirty = false;
    ++version;
}

std::span<const std::array<float, 16>> SkeletonPose::GetPalette(const std::uint32_t skin_index) const {
    if (skin_index >= palettes.size()) return {};
    return palettes[skin_index];
}

} // namespace VulkanEngine::Components
