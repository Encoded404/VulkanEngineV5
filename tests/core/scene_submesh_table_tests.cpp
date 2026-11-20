#include <gtest/gtest.h>

import std;

import VulkanEngine.SceneRenderer;
import VulkanEngine.Mesh.MeshTypes;

namespace {

using VulkanEngine::AdjustSubmeshesForIndexBlock;
using VulkanEngine::AppendAdjustedSubmeshes;
using VulkanEngine::MaterialId;
using VulkanEngine::SceneRenderer::SceneRenderer;
using VulkanEngine::SubMesh;

SubMesh MakeSubMesh(const std::uint32_t index_start, const std::uint32_t index_count,
                    const MaterialId material = MaterialId{}) {
    SubMesh submesh{};
    submesh.index_start = index_start;
    submesh.index_count = index_count;
    submesh.material_id = material;
    return submesh;
}

// Compares the whole table, not just the entries a case touched.
void ExpectTableMatches(const SceneRenderer& renderer, const std::vector<SubMesh>& expected) {
    const std::span<const SubMesh> view = renderer.GetSubmeshes();
    EXPECT_EQ(view.size(), expected.size());
    if (view.size() != expected.size()) return;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(view[i].index_start, expected[i].index_start);
        EXPECT_EQ(view[i].index_count, expected[i].index_count);
        EXPECT_EQ(view[i].material_id, expected[i].material_id);
    }
}

// One mesh as the registry and the gather pass see it:
// where its range starts.
// how many entries it covers.
// the local submeshes the range was built from.
struct MeshEntry {
    std::uint32_t index_offset;
    std::uint32_t total_index_count;
    std::vector<SubMesh> local;
    std::uint32_t first{0};
    std::uint32_t count{0};
};

// This file tests the scene submesh table without a device, so the cases run in the default test set.
// A renderer that never ran Initialize still owns the vector, and the methods below only touch that vector.
//
// The cases pin the slot arithmetic gather depends on, not the table-wide copy the change removed:
// a copy-and-replace append would still satisfy them, and capacity is unspecified, so an in-place append is not asserted here.

TEST(SceneSubmeshTableTest, GetSubmeshesIsAReadOnlyView) {
    static_assert(std::is_same_v<
                  decltype(std::declval<const SceneRenderer&>().GetSubmeshes()),
                  std::span<const SubMesh>>);
}

TEST(AdjustSubmeshesForIndexBlockTest, RebasesIndexStartsAndKeepsTheRest) {
    const std::array<SubMesh, 2> local{
        MakeSubMesh(0, 3, MaterialId{7}), MakeSubMesh(10, 5, MaterialId{8})};

    const std::vector<SubMesh> adjusted = AdjustSubmeshesForIndexBlock(local, 64, 15);

    ASSERT_EQ(adjusted.size(), 2u);
    EXPECT_EQ(adjusted[0].index_start, 64u);
    EXPECT_EQ(adjusted[0].index_count, 3u);
    EXPECT_EQ(adjusted[0].material_id, MaterialId{7});
    EXPECT_EQ(adjusted[1].index_start, 74u);
    EXPECT_EQ(adjusted[1].index_count, 5u);
    EXPECT_EQ(adjusted[1].material_id, MaterialId{8});
}

TEST(AdjustSubmeshesForIndexBlockTest, EmptyRangeIsOneWindowOverTheWholeBlock) {
    const std::span<const SubMesh> empty{};

    // The fallback logs an error in debug builds because the window has no vertex span.
    const std::vector<SubMesh> adjusted = AdjustSubmeshesForIndexBlock(empty, 512, 48);

    ASSERT_EQ(adjusted.size(), 1u);
    EXPECT_EQ(adjusted[0].index_start, 512u);
    EXPECT_EQ(adjusted[0].index_count, 48u);
}

TEST(AdjustSubmeshesForIndexBlockTest, AppendFormReusesTheDestinationAndReturnsTheFirstSlot) {
    std::vector<SubMesh> out{MakeSubMesh(1, 2)};
    out.reserve(4);
    const SubMesh* const before = out.data();
    const std::array<SubMesh, 2> local{
        MakeSubMesh(0, 3, MaterialId{7}), MakeSubMesh(10, 5, MaterialId{8})};

    EXPECT_EQ(AppendAdjustedSubmeshes(out, local, 64, 15), 1u);

    // The destination had room for the whole result, so the append had to stay in this allocation.
    EXPECT_EQ(out.data(), before);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].index_start, 1u);
    EXPECT_EQ(out[1].index_start, 64u);
    EXPECT_EQ(out[1].index_count, 3u);
    EXPECT_EQ(out[1].material_id, MaterialId{7});
    EXPECT_EQ(out[2].index_start, 74u);
    EXPECT_EQ(out[2].index_count, 5u);
    EXPECT_EQ(out[2].material_id, MaterialId{8});
}

TEST(AdjustSubmeshesForIndexBlockTest, AppendFormAddsOneWindowForAnEmptyRange) {
    std::vector<SubMesh> out{MakeSubMesh(1, 2)};
    const std::span<const SubMesh> empty{};

    // The fallback logs an error in debug builds because the window has no vertex span.
    EXPECT_EQ(AppendAdjustedSubmeshes(out, empty, 512, 48), 1u);

    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].index_start, 1u);
    EXPECT_EQ(out[1].index_start, 512u);
    EXPECT_EQ(out[1].index_count, 48u);
    EXPECT_EQ(out[1].material_id, MaterialId{});
}

TEST(SceneSubmeshTableTest, RangesTileTheTableAndResolveToTheirOwnSubmeshes) {
    // A mesh with one submesh, one with three, and one with none (a single window over its whole index block).
    const std::array<MeshEntry, 3> meshes{{
        {100, 30, {MakeSubMesh(0, 30, MaterialId{1})}},
        {500, 27, {MakeSubMesh(4, 5), MakeSubMesh(9, 7), MakeSubMesh(16, 11)}},
        {900, 48, {}},
    }};

    SceneRenderer renderer;
    std::vector<SubMesh> expected;
    std::vector<MeshEntry> loaded;
    for (const MeshEntry& mesh : meshes) {
        const std::vector<SubMesh> adjusted = AdjustSubmeshesForIndexBlock(
            mesh.local, mesh.index_offset, mesh.total_index_count);

        MeshEntry entry = mesh;
        entry.first = renderer.AppendSubmeshes(adjusted);
        entry.count = static_cast<std::uint32_t>(adjusted.size());

        // The next mesh starts where this range ends.
        EXPECT_EQ(entry.first, static_cast<std::uint32_t>(expected.size()));
        expected.insert(expected.end(), adjusted.begin(), adjusted.end());
        loaded.push_back(entry);
    }

    ExpectTableMatches(renderer, expected);

    // Gather resolves each mesh as first + s; every slot must land on that mesh's own submesh, re-based onto its index block.
    const std::span<const SubMesh> view = renderer.GetSubmeshes();
    for (const MeshEntry& mesh : loaded) {
        ASSERT_LE(mesh.first + mesh.count, view.size());
        for (std::uint32_t s = 0; s < mesh.count; ++s) {
            SCOPED_TRACE(::testing::Message() << "first=" << mesh.first << " s=" << s);
            const SubMesh& resolved = view[mesh.first + s];
            if (mesh.local.empty()) {
                EXPECT_EQ(resolved.index_start, mesh.index_offset);
                EXPECT_EQ(resolved.index_count, mesh.total_index_count);
            } else {
                EXPECT_EQ(resolved.index_start, mesh.local[s].index_start + mesh.index_offset);
                EXPECT_EQ(resolved.index_count, mesh.local[s].index_count);
                EXPECT_EQ(resolved.material_id, mesh.local[s].material_id);
            }
        }
    }
}

TEST(SceneSubmeshTableTest, AppendPastCapacityKeepsEveryEntry) {
    SceneRenderer renderer;
    const std::array<SubMesh, 2> prefix{MakeSubMesh(1, 2), MakeSubMesh(3, 4)};
    ASSERT_EQ(renderer.AppendSubmeshes(prefix), 0u);

    // 64 more entries force the table to grow, so an append that rebuilds it or drops the prefix would show here.
    std::vector<SubMesh> tail;
    for (std::uint32_t i = 0; i < 64; ++i) {
        tail.push_back(MakeSubMesh(100u + i, 1));
    }
    EXPECT_EQ(renderer.AppendSubmeshes(tail), 2u);

    std::vector<SubMesh> expected(prefix.begin(), prefix.end());
    expected.insert(expected.end(), tail.begin(), tail.end());
    ExpectTableMatches(renderer, expected);
}

TEST(SceneSubmeshTableTest, AppendEmptyRangeReturnsTheCurrentEnd) {
    SceneRenderer renderer;
    const std::array<SubMesh, 1> one{MakeSubMesh(4, 8)};
    ASSERT_EQ(renderer.AppendSubmeshes(one), 0u);

    const std::span<const SubMesh> empty{};
    EXPECT_EQ(renderer.AppendSubmeshes(empty), 1u);
    ExpectTableMatches(renderer, {one[0]});
}

TEST(SceneSubmeshTableTest, SetSubmeshesReplacesTheTableAndRestartsAppends) {
    SceneRenderer renderer;
    const std::array<SubMesh, 2> before{MakeSubMesh(0, 3), MakeSubMesh(3, 6)};
    ASSERT_EQ(renderer.AppendSubmeshes(before), 0u);

    // A scene load replaces the whole table.
    renderer.SetSubmeshes({MakeSubMesh(7, 9)});
    ExpectTableMatches(renderer, {MakeSubMesh(7, 9)});

    // The next lazily resident mesh appends after the replacement, not after the table the load discarded.
    const std::array<SubMesh, 1> after_load{MakeSubMesh(20, 4)};
    EXPECT_EQ(renderer.AppendSubmeshes(after_load), 1u);
    ExpectTableMatches(renderer, {MakeSubMesh(7, 9), MakeSubMesh(20, 4)});
}

TEST(SceneSubmeshTableTest, SetSubmeshesToEmptyLeavesAnAppendableTable) {
    SceneRenderer renderer;
    const std::array<SubMesh, 1> one{MakeSubMesh(4, 8)};
    ASSERT_EQ(renderer.AppendSubmeshes(one), 0u);

    renderer.SetSubmeshes({});
    EXPECT_TRUE(renderer.GetSubmeshes().empty());

    EXPECT_EQ(renderer.AppendSubmeshes(one), 0u);
    ExpectTableMatches(renderer, {one[0]});
}

} // namespace
