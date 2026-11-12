// Device-free tests for the GeometryModifier contract that is not stack
// resolution: mesh fingerprint adoption and the bounds policy.
//
// The bounds policy matters because a deform moves vertices away from the asset's
// rest-pose bounds, so the culler may not trust them. No envelope of a deformed
// object exists yet, so every policy except SkipCulling has to be reported as
// unsupported and ignored; gather keeps both cull bits set unconditionally.

#include <gtest/gtest.h>

import std;

import VulkanEngine.Components.GeometryModifier;

namespace {

using VulkanEngine::Components::BoundsPolicy;
using VulkanEngine::Components::BoundsPolicyName;
using VulkanEngine::Components::GeometryModifier;
using VulkanEngine::Components::UnsupportedBoundsPolicy;

TEST(GeometryModifierTest, DefaultBoundsPolicyIsSkipCulling) {
    // A modifier moves vertices away from their rest-pose bounds, so the default
    // must not let the culler trust them.
    const GeometryModifier modifier{};
    EXPECT_EQ(modifier.bounds, BoundsPolicy::SkipCulling);
    EXPECT_FALSE(UnsupportedBoundsPolicy(modifier.bounds).has_value())
        << "the default must not be reported as an unsupported request";
}

TEST(GeometryModifierTest, SkipCullingIsTheOnlyHonouredPolicy) {
    // SkipCulling is what gather implements, so it needs no diagnostic.
    EXPECT_FALSE(UnsupportedBoundsPolicy(BoundsPolicy::SkipCulling).has_value());

    // RestPose would cull against bounds that no longer hold.
    const auto rest_pose = UnsupportedBoundsPolicy(BoundsPolicy::RestPose);
    ASSERT_TRUE(rest_pose.has_value());
    EXPECT_EQ(*rest_pose, BoundsPolicy::RestPose);

    // Conservative and Manual need a deformed envelope or caller bounds that do
    // not exist yet; both fall back to SkipCulling and are reported.
    const auto conservative = UnsupportedBoundsPolicy(BoundsPolicy::Conservative);
    ASSERT_TRUE(conservative.has_value());
    EXPECT_EQ(*conservative, BoundsPolicy::Conservative);
    const auto manual = UnsupportedBoundsPolicy(BoundsPolicy::Manual);
    ASSERT_TRUE(manual.has_value());
    EXPECT_EQ(*manual, BoundsPolicy::Manual);
}

TEST(GeometryModifierTest, BoundsPolicyNamesAreDistinctAndDiagnostic) {
    const std::array<BoundsPolicy, 4> policies{
        BoundsPolicy::RestPose,
        BoundsPolicy::SkipCulling,
        BoundsPolicy::Conservative,
        BoundsPolicy::Manual,
    };
    std::set<std::string> names;
    for (const BoundsPolicy policy : policies) {
        const std::string_view name = BoundsPolicyName(policy);
        EXPECT_FALSE(name.empty());
        names.emplace(name);
    }
    EXPECT_EQ(names.size(), policies.size()) << "each policy needs its own diagnostic spelling";
    EXPECT_EQ(BoundsPolicyName(BoundsPolicy::SkipCulling), "SkipCulling");
}

TEST(GeometryModifierTest, AnUnsupportedPolicyIsReportedOncePerPolicy) {
    GeometryModifier modifier{};
    modifier.bounds = BoundsPolicy::Conservative;

    // gather reports through the component's own once-per-cause set, so the same
    // request logs once and a different request logs again.
    const auto signature = [](const BoundsPolicy policy) {
        return static_cast<std::uint64_t>(UnsupportedBoundsPolicy(policy).value());
    };
    EXPECT_TRUE(modifier.ShouldReport(signature(BoundsPolicy::Conservative)));
    EXPECT_FALSE(modifier.ShouldReport(signature(BoundsPolicy::Conservative)));

    modifier.bounds = BoundsPolicy::RestPose;
    EXPECT_TRUE(modifier.ShouldReport(signature(BoundsPolicy::RestPose)));

    // Re-binding the component (a new mesh) re-arms the diagnostics.
    modifier.Bind(7);
    EXPECT_TRUE(modifier.ShouldReport(signature(BoundsPolicy::RestPose)));
}

TEST(GeometryModifierTest, MeshIdAdoptsOnceAndReportsAMismatchOnce) {
    GeometryModifier modifier{};
    EXPECT_EQ(modifier.mesh_id, GeometryModifier::kUnboundMesh);

    // Gather adopts the owner's mesh on first use.
    modifier.mesh_id = 42;
    EXPECT_EQ(modifier.mesh_id, 42u);

    // A diagnostic is reported once per distinct signature, not per frame.
    EXPECT_TRUE(modifier.ShouldReport(0x1234));
    EXPECT_FALSE(modifier.ShouldReport(0x1234));
    EXPECT_TRUE(modifier.ShouldReport(0x5678));

    // Re-binding clears the diagnostic state so a fresh mismatch is reported.
    modifier.Bind(43);
    EXPECT_EQ(modifier.mesh_id, 43u);
    EXPECT_TRUE(modifier.ShouldReport(0x1234));

    // Unbinding returns the component to "adopt on first use".
    modifier.Unbind();
    EXPECT_EQ(modifier.mesh_id, GeometryModifier::kUnboundMesh);
    EXPECT_TRUE(modifier.ShouldReport(0x1234));
}

} // namespace
