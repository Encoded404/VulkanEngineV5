#include <gtest/gtest.h>

import std;

import VulkanEngine.MaterialManager;

namespace {

using VulkanEngine::MaterialManager::MaterialEntry;
using VulkanEngine::MaterialManager::MaterialId;
using VulkanEngine::MaterialManager::MaterialManager;

// Builds a manager with one synthetic material whose payload is a technique's
// declared texture-slot words. Mirrors how a real technique's
// TextureSlotFieldOffsets() drives the residency rewrite.
struct Fixture {
    MaterialManager mgr;
    static constexpr std::uint32_t kTechnique = 3;
    static constexpr std::uint32_t kAlbedoOffset = 0;
    static constexpr std::uint32_t kNormalOffset = 4;

    explicit Fixture(std::uint32_t albedo, std::uint32_t normal) {
        std::array<std::uint32_t, 2> fields{kAlbedoOffset, kNormalOffset};
        mgr.SetTextureSlotFields(kTechnique, fields);

        auto entry = std::make_unique<MaterialEntry>();
        entry->technique_id = VulkanEngine::TechniqueManager::TechniqueId{kTechnique};
        entry->cpu_data.resize(8);
        std::memcpy(entry->cpu_data.data() + kAlbedoOffset, &albedo, 4);
        std::memcpy(entry->cpu_data.data() + kNormalOffset, &normal, 4);
        if (mgr.Materials.empty()) {
            mgr.Materials.emplace_back();
            mgr.generations_.emplace_back(1);
        }
        mgr.Materials[0] = std::move(entry);
    }

    [[nodiscard]] std::uint32_t Word(std::uint32_t offset) const {
        std::uint32_t value = 0;
        std::memcpy(&value, mgr.Materials[0]->cpu_data.data() + offset, 4);
        return value;
    }
};

// Every slot word equal to the evicted slot is rewritten to the fallback.
TEST(MaterialSlotRewriteTest, RewritesMatchingSlots) {
    Fixture fixture(/*albedo=*/7, /*normal=*/7);
    fixture.mgr.RebuildTextureSlotIndex();
    fixture.mgr.RewriteTextureSlot(7, /*replacement=*/0);
    EXPECT_EQ(fixture.Word(Fixture::kAlbedoOffset), 0u);
    EXPECT_EQ(fixture.Word(Fixture::kNormalOffset), 0u);
}

// Only the matching slot moves; an unrelated slot is left alone.
TEST(MaterialSlotRewriteTest, LeavesUnrelatedSlots) {
    Fixture fixture(/*albedo=*/7, /*normal=*/9);
    fixture.mgr.RebuildTextureSlotIndex();
    fixture.mgr.RewriteTextureSlot(7, 0);
    EXPECT_EQ(fixture.Word(Fixture::kAlbedoOffset), 0u);
    EXPECT_EQ(fixture.Word(Fixture::kNormalOffset), 9u);
}

// Rewriting a slot no material references is a no-op.
TEST(MaterialSlotRewriteTest, NoUsersIsNoOp) {
    Fixture fixture(/*albedo=*/7, /*normal=*/7);
    fixture.mgr.RebuildTextureSlotIndex();
    fixture.mgr.RewriteTextureSlot(1234, 0);
    EXPECT_EQ(fixture.Word(Fixture::kAlbedoOffset), 7u);
    EXPECT_EQ(fixture.Word(Fixture::kNormalOffset), 7u);
}

// The index reports the material as a user of both referenced slots.
TEST(MaterialSlotRewriteTest, IndexFindsUsers) {
    Fixture fixture(/*albedo=*/7, /*normal=*/9);
    fixture.mgr.RebuildTextureSlotIndex();
    int albedo_users = 0;
    fixture.mgr.ForEachMaterialUsingSlot(7, [&](MaterialId) { ++albedo_users; });
    int normal_users = 0;
    fixture.mgr.ForEachMaterialUsingSlot(9, [&](MaterialId) { ++normal_users; });
    EXPECT_EQ(albedo_users, 1);
    EXPECT_EQ(normal_users, 1);
}

}  // namespace
