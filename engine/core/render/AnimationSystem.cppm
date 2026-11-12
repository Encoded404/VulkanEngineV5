module;

export module VulkanEngine.AnimationSystem;

import std;

export import VulkanEngine.ECS.ComponentRegistry;
export import VulkanEngine.Components.AnimationPlayer;
export import VulkanEngine.Components.SkeletonPose;

export namespace VulkanEngine {

// Clip playback for `SkeletonPose` objects, device-free.
//
// This is the CPU half of skeletal animation: it advances each playing
// `AnimationPlayer`, samples the clip into the pose's `local` array, and rebuilds
// world transforms and skin palettes so gather copies a finished pose. It owns no
// device object and no per-frame state, so it is testable without a Vulkan device
// and its whole job fits in one `Update` call.
//
// It must run before `MeshProcessFrame` (gather copies `SkeletonPose::palettes`,
// so sampling afterwards would render the previous frame's pose). GameEngine
// registers it in `FrameSlot::AnimationUpdate` with an explicit before-gather
// constraint.
class AnimationSystem {
public:
    // Advances every player, samples its clip and rebuilds the pose it drives;
    // poses with no player rebuild the bind pose while `dirty`. Returns the number
    // of poses rebuilt, which is what tests and profiling assert on.
    //
    // `delta_time` is the rendered-frame delta: animation is presentation-rate
    // work, and a fixed-step delta would freeze playback when fixed stepping is
    // disabled or run it at simulation rate when the render rate drops.
    std::size_t Update(ComponentRegistry& registry, float delta_time);
};

} // namespace VulkanEngine
