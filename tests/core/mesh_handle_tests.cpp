// Device-free tests for mesh handle identity and slot reuse.
//
// The handle is what every component stores to decide whether an authored
// override still belongs to the mesh it was written for, so a released slot that
// comes back must never revive an old handle.

#include <gtest/gtest.h>

import std;

import VulkanEngine.GpuResources.MeshData;
import VulkanEngine.MeshHandle;
import VulkanEngine.MeshManager;
import VulkanEngine.MeshRegistry;

namespace {

using VulkanEngine::GpuResources::MeshData;
using VulkanEngine::MeshHandle;
using VulkanEngine::MeshManager;
using VulkanEngine::MeshRegistry;

TEST(MeshHandleTest, ADefaultHandleNamesNoMesh) {
    const MeshHandle unset{};
    EXPECT_FALSE(unset.IsValid());

    const MeshHandle live{0, 1};
    EXPECT_TRUE(live.IsValid());
}

TEST(MeshHandleTest, TheFirstRegistrationIsSlotZeroGenerationOne) {
    MeshRegistry registry;

    const MeshHandle handle = registry.Register(MeshData{});

    EXPECT_EQ(handle.slot, 0u);
    EXPECT_EQ(handle.generation, 1u);
    EXPECT_TRUE(handle.IsValid());
    EXPECT_NE(registry.Get(handle), nullptr);
}

TEST(MeshHandleTest, AHandlePastTheEndFindsNothing) {
    MeshRegistry registry;

    EXPECT_EQ(registry.Get(MeshHandle{3, 1}), nullptr);
}

TEST(MeshHandleTest, ReleaseThenRegisterReusesTheSlotWithANewGeneration) {
    MeshRegistry registry;
    MeshManager mgr;

    const MeshHandle first = registry.Register(MeshData{});
    registry.Release(first, mgr);
    const MeshHandle second = registry.Register(MeshData{});

    EXPECT_EQ(second.slot, first.slot);
    EXPECT_NE(second.generation, first.generation);
    EXPECT_NE(second.Signature(), first.Signature());
}

TEST(MeshHandleTest, AStaleHandleFindsNothingOnceItsSlotIsReused) {
    MeshRegistry registry;
    MeshManager mgr;

    const MeshHandle stale = registry.Register(MeshData{});
    registry.Release(stale, mgr);
    EXPECT_EQ(registry.Get(stale), nullptr) << "a released mesh names nothing";

    const MeshHandle live = registry.Register(MeshData{});
    EXPECT_NE(registry.Get(live), nullptr);
    EXPECT_EQ(registry.Get(stale), nullptr) << "the slot now holds a later registration";
}

TEST(MeshHandleTest, ReleasingTwiceFreesTheSlotOnce) {
    MeshRegistry registry;
    MeshManager mgr;

    const MeshHandle first = registry.Register(MeshData{});
    registry.Release(first, mgr);
    registry.Release(first, mgr);

    const MeshHandle second = registry.Register(MeshData{});
    const MeshHandle third = registry.Register(MeshData{});
    EXPECT_EQ(second.slot, first.slot);
    EXPECT_NE(third.slot, second.slot) << "a double release must not hand the slot out twice";
}

TEST(MeshHandleTest, AStaleReleaseLeavesTheReusingMeshAlive) {
    MeshRegistry registry;
    MeshManager mgr;

    const MeshHandle stale = registry.Register(MeshData{});
    registry.Release(stale, mgr);
    const MeshHandle live = registry.Register(MeshData{});

    registry.Release(stale, mgr);

    EXPECT_NE(registry.Get(live), nullptr)
        << "a handle from before the reuse must not release the new mesh";
}

} // namespace
