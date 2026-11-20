#include <gtest/gtest.h>

import std;

import vulkan;
import test_gpu;
import TestSupport.HeadlessVulkanBackend;
import VulkanEngine.GpuResources.StagingPool;
import VulkanEngine.GpuResources.DeviceBufferHeap;
import VulkanEngine.GpuResources.MeshData;
import VulkanEngine.MeshManager;
import VulkanEngine.MeshHandle;
import VulkanEngine.MeshRegistry;
import VulkanEngine.MeshRenderSystem;
import VulkanEngine.SceneRenderer;
import VulkanEngine.SceneGpuLayout;
import VulkanEngine.SceneLimits;
import VulkanEngine.StandardMeshPipeline;
import VulkanEngine.Mesh.MeshTypes;

namespace {

using VulkanEngine::GpuBoneInfluence;
using VulkanEngine::MeshHandle;
using VulkanEngine::MeshManager;
using VulkanEngine::MeshRegistry;
using VulkanEngine::MeshVertexVec2;
using VulkanEngine::SceneGpuLayout::TableId;
using VulkanEngine::SceneLimits::kMaxSkinBuffers;
using VulkanEngine::SceneRenderer::SceneRenderer;
using VulkanEngine::GpuResources::DeviceBufferHeap;
using VulkanEngine::GpuResources::HeapConfig;
using VulkanEngine::GpuResources::MeshData;
using VulkanEngine::GpuResources::StagingPool;
using VulkanEngine::GpuResources::StagingPoolConfig;
using VulkanEngine::StandardMeshPipeline::Vertex;

// Each test drives the real staging pool and the real heaps, so a batch is measured by the submits it makes.
class MeshUploadBatchGpuTest : public ::testing::Test {
protected:
    // A 3600-byte influence stream fills a 4 KiB block on its own, so mesh N claims block N - 1.
    static constexpr std::uint64_t kSkinBlockBytes = 4ULL << 10;
    static constexpr std::uint32_t kSkinVertexCount = 300;
    // MeshManager allocates the stream on a 48-byte boundary so a region base divides evenly into 12-byte records.
    static constexpr std::uint64_t kSkinAlignment = 48;
    static constexpr std::uint64_t kSkinStreamBytes =
        kSkinVertexCount * sizeof(GpuBoneInfluence);
    static_assert(kSkinStreamBytes <= kSkinBlockBytes,
                  "the skinned fixture mesh has to fit in one skin block");
    static_assert(2 * kSkinStreamBytes > kSkinBlockBytes,
                  "two fixture streams must not share a block, or a mesh's block index would not be its mesh index");
    // The first block the deform table cannot bind is kMaxSkinBuffers, so this many meshes are what defer a free.
    static constexpr std::uint32_t kCappedMeshCount = kMaxSkinBuffers + 1;

    TestSupport::HeadlessVulkanBackend backend{};
    DeviceBufferHeap vertex_heap{};
    DeviceBufferHeap index_heap{};
    DeviceBufferHeap uv_heap{};
    DeviceBufferHeap skin_heap{};
    StagingPool staging{};
    // Only persistent uploads run here, so the streamed heaps stay uninitialized; the manager just stores the pointers.
    std::vector<DeviceBufferHeap> dynamic_vertex_heaps{};
    std::vector<DeviceBufferHeap> dynamic_index_heaps{};
    MeshManager mesh_mgr{};
    MeshRegistry registry{};
    SceneRenderer renderer{};

    void SetUp() override {
        if (!TestSupport::IsGpuDeviceAvailable()) {
            GTEST_SKIP() << "no Vulkan device available";
        }
        ASSERT_TRUE(backend.Initialize());

        ASSERT_TRUE(vertex_heap.Initialize(backend, HeapConfig{.block_size = 16ULL << 20},
                                           "batch-test-vertex"));
        ASSERT_TRUE(index_heap.Initialize(backend, HeapConfig{.block_size = 8ULL << 20},
                                          "batch-test-index"));
        ASSERT_TRUE(uv_heap.Initialize(backend, HeapConfig{.block_size = 8ULL << 20},
                                       "batch-test-uv"));
        ASSERT_TRUE(skin_heap.Initialize(backend, HeapConfig{.block_size = kSkinBlockBytes},
                                         "batch-test-skin"));

        const std::uint32_t frames_in_flight = backend.GetFramesInFlight();
        dynamic_vertex_heaps.resize(frames_in_flight);
        dynamic_index_heaps.resize(frames_in_flight);

        StagingPoolConfig staging_config{};
        staging_config.block_size = 1ULL << 20;
        // Small enough that the capacity case has to close its group at least twice.
        staging_config.capacity_bytes = 4ULL << 20;
        ASSERT_TRUE(staging.Initialize(backend, staging_config));

        ASSERT_TRUE(mesh_mgr.Initialize(backend, &vertex_heap, &index_heap, &uv_heap, &skin_heap,
                                        &staging, dynamic_vertex_heaps.data(),
                                        dynamic_index_heaps.data(), frames_in_flight));
    }

    void TearDown() override {
        if (!TestSupport::IsGpuDeviceAvailable()) {
            return;
        }
        registry.Shutdown();
        mesh_mgr.Shutdown();
        staging.Shutdown();
        skin_heap.Shutdown();
        uv_heap.Shutdown();
        index_heap.Shutdown();
        vertex_heap.Shutdown();
        backend.Shutdown();
    }

    // A quad: four vertices, two triangles, one submesh.
    static MeshData MakeQuad(const bool with_uv1 = false) {
        MeshData data{};
        data.vertices = {
            Vertex{.px = 0.0f, .py = 0.0f, .pz = 0.0f, .packedTBN = 0, .u = 0.0f, .v = 0.0f},
            Vertex{.px = 1.0f, .py = 0.0f, .pz = 0.0f, .packedTBN = 0, .u = 1.0f, .v = 0.0f},
            Vertex{.px = 1.0f, .py = 1.0f, .pz = 0.0f, .packedTBN = 0, .u = 1.0f, .v = 1.0f},
            Vertex{.px = 0.0f, .py = 1.0f, .pz = 0.0f, .packedTBN = 0, .u = 0.0f, .v = 1.0f},
        };
        data.indices = {0, 1, 2, 0, 2, 3};
        data.sub_meshes.push_back(VulkanEngine::SubMesh{.index_start = 0, .index_count = 6});
        if (with_uv1) {
            data.uv_extra[0].assign(data.vertices.size(), MeshVertexVec2{0.5f, 0.5f});
        }
        return data;
    }

    // A mesh whose influence stream takes 300 * 12 = 3600 bytes, so it claims a whole 4 KiB skin block.
    static MeshData MakeSkinnedMesh(const std::uint32_t vertex_count = kSkinVertexCount) {
        MeshData data{};
        data.vertices.resize(vertex_count);
        data.indices.resize(vertex_count);
        for (std::uint32_t i = 0; i < vertex_count; ++i) {
            data.vertices[i].px = static_cast<float>(i);
            data.indices[i] = i;
        }
        data.sub_meshes.push_back(VulkanEngine::SubMesh{
            .index_start = 0, .index_count = vertex_count});
        data.bone_weights.assign(vertex_count, VulkanEngine::BoneWeight{});
        data.influence_sets = 1;
        return data;
    }

    // A mesh with no skin, large enough that a few of them overflow the staging ceiling.
    static MeshData MakeLargeMesh(const std::uint32_t vertex_count = 8000) {
        MeshData data{};
        data.vertices.resize(vertex_count);
        data.indices.resize(vertex_count);
        for (std::uint32_t i = 0; i < vertex_count; ++i) {
            data.vertices[i].px = static_cast<float>(i);
            data.indices[i] = i;
        }
        data.sub_meshes.push_back(VulkanEngine::SubMesh{
            .index_start = 0, .index_count = vertex_count});
        return data;
    }
};

// A batch submits the copies of every mesh in it once, not once per mesh.
TEST_F(MeshUploadBatchGpuTest, OneBatchCostsOneFlush) {
    const std::uint64_t before = staging.ImmediateFlushCount();

    std::vector<MeshManager::Handle> handles;
    {
        MeshManager::UploadBatchScope batch(mesh_mgr);
        for (int i = 0; i < 8; ++i) {
            handles.push_back(mesh_mgr.UploadPersistent(MakeQuad()));
        }
    }

    EXPECT_EQ(staging.ImmediateFlushCount(), before + 1);
    for (const MeshManager::Handle handle : handles) {
        EXPECT_TRUE(handle.IsValid());
        EXPECT_NE(mesh_mgr.GetMeshInfo(handle), nullptr);
    }
}

// Without a group each upload keeps its own submit and wait, which is the behavior the batch replaces.
TEST_F(MeshUploadBatchGpuTest, StandaloneUploadsCostOneFlushEach) {
    const std::uint64_t before = staging.ImmediateFlushCount();

    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(mesh_mgr.UploadPersistent(MakeQuad()).IsValid());
    }

    EXPECT_EQ(staging.ImmediateFlushCount(), before + 3);
}

// A group that recorded nothing has nothing to submit, and the counter must not move for it.
TEST_F(MeshUploadBatchGpuTest, EmptyBatchDoesNotFlush) {
    const std::uint64_t before = staging.ImmediateFlushCount();
    {
        MeshManager::UploadBatchScope batch(mesh_mgr);
    }
    EXPECT_EQ(staging.ImmediateFlushCount(), before);
}

// Nested scopes close only when the outermost one ends, which is what the manager's depth counter defines.
TEST_F(MeshUploadBatchGpuTest, NestedScopesCloseAtTheOutermostEnd) {
    const std::uint64_t before = staging.ImmediateFlushCount();

    std::vector<MeshManager::Handle> handles;
    {
        MeshManager::UploadBatchScope outer(mesh_mgr);
        handles.push_back(mesh_mgr.UploadPersistent(MakeQuad()));
        {
            MeshManager::UploadBatchScope inner(mesh_mgr);
            handles.push_back(mesh_mgr.UploadPersistent(MakeQuad()));
        }
        // The inner scope ended, but the outer one still owns the close.
        EXPECT_EQ(staging.ImmediateFlushCount(), before);
        handles.push_back(mesh_mgr.UploadPersistent(MakeQuad()));
        EXPECT_EQ(staging.ImmediateFlushCount(), before);
    }

    EXPECT_EQ(staging.ImmediateFlushCount(), before + 1);
    for (const MeshManager::Handle handle : handles) {
        EXPECT_TRUE(handle.IsValid());
    }
}

// An end without a matching begin is refused, so the depth cannot underflow into a permanently open batch.
TEST_F(MeshUploadBatchGpuTest, UnmatchedEndKeepsTheDepthAtZero) {
    const std::uint64_t before = staging.ImmediateFlushCount();
    mesh_mgr.EndUploadBatch();

    EXPECT_TRUE(mesh_mgr.UploadPersistent(MakeQuad()).IsValid());
    EXPECT_EQ(staging.ImmediateFlushCount(), before + 1);
}

// A batch that needs more staging than the ceiling still uploads every mesh: it closes the group and carries on.
TEST_F(MeshUploadBatchGpuTest, BackPressureClosesTheGroupAndSucceeds) {
    const std::uint64_t before = staging.ImmediateFlushCount();

    std::vector<MeshManager::Handle> handles;
    {
        MeshManager::UploadBatchScope batch(mesh_mgr);
        for (int i = 0; i < 40; ++i) {
            handles.push_back(mesh_mgr.UploadPersistent(MakeLargeMesh()));
        }
    }

    for (const MeshManager::Handle handle : handles) {
        EXPECT_TRUE(handle.IsValid());
    }
    EXPECT_GE(staging.ImmediateFlushCount(), before + 2);
    EXPECT_EQ(staging.OutstandingBytes(), 0u);
}

// The skin ranges of a mesh that trips the deform cap are freed only after the group flush.
// A range freed while its copy is still queued is handed to the next mesh, whose copy then overwrites it.
TEST_F(MeshUploadBatchGpuTest, AbortedSkinUploadDefersItsHeapFrees) {
    std::vector<MeshManager::Handle> handles;
    {
        MeshManager::UploadBatchScope batch(mesh_mgr);
        for (std::uint32_t i = 0; i < kCappedMeshCount; ++i) {
            handles.push_back(mesh_mgr.UploadPersistent(MakeSkinnedMesh()));
        }
        // Only the last mesh deferred a range, and it is still pending while the group is open.
        EXPECT_EQ(mesh_mgr.GetPendingHeapFreeCount(), 1u);

        const auto* capped = mesh_mgr.GetMeshInfo(handles.back());
        ASSERT_NE(capped, nullptr);
        ASSERT_FALSE(capped->skin_allocation.IsValid());

        // The deferred range must still be allocated, so this exact-size probe cannot take the capped mesh's block.
        // Every earlier block holds one stream and has no room left, so an early free would show up as a hit here.
        const auto early_take = skin_heap.Allocate(kSkinStreamBytes, kSkinAlignment);
        ASSERT_TRUE(early_take.IsValid());
        EXPECT_NE(early_take.buffer_index, kMaxSkinBuffers);
    }
    EXPECT_EQ(mesh_mgr.GetPendingHeapFreeCount(), 0u);

    // The close returned the range to the allocator, in the block the capped mesh had allocated.
    const auto after_close = skin_heap.Allocate(kSkinStreamBytes, kSkinAlignment);
    ASSERT_TRUE(after_close.IsValid());
    EXPECT_EQ(after_close.buffer_index, kMaxSkinBuffers);

    ASSERT_EQ(handles.size(), static_cast<std::size_t>(kCappedMeshCount));
    EXPECT_TRUE(handles.back().IsValid());
    const auto* capped = mesh_mgr.GetMeshInfo(handles.back());
    ASSERT_NE(capped, nullptr);
    EXPECT_EQ(capped->influence_sets, 0u);
    const auto* resident = mesh_mgr.GetMeshInfo(handles[handles.size() - 2]);
    ASSERT_NE(resident, nullptr);
    EXPECT_EQ(resident->influence_sets, 1u);
    ASSERT_TRUE(resident->skin_allocation.IsValid());
    EXPECT_EQ(resident->skin_allocation.buffer_index, kMaxSkinBuffers - 1);
}

// A close forced by the staging ceiling releases deferred ranges exactly like the final close does.
TEST_F(MeshUploadBatchGpuTest, CapacityCloseReleasesPendingFrees) {
    const std::uint64_t before = staging.ImmediateFlushCount();

    std::vector<MeshManager::Handle> handles;
    {
        MeshManager::UploadBatchScope batch(mesh_mgr);
        for (std::uint32_t i = 0; i < kCappedMeshCount; ++i) {
            handles.push_back(mesh_mgr.UploadPersistent(MakeSkinnedMesh()));
        }
        ASSERT_EQ(mesh_mgr.GetPendingHeapFreeCount(), 1u);

        // These overrun the 4 MiB ceiling, so a close happens before the scope ends.
        for (int i = 0; i < 40; ++i) {
            handles.push_back(mesh_mgr.UploadPersistent(MakeLargeMesh()));
        }
        EXPECT_GE(staging.ImmediateFlushCount(), before + 2);
        EXPECT_EQ(mesh_mgr.GetPendingHeapFreeCount(), 0u);
    }

    EXPECT_EQ(staging.OutstandingBytes(), 0u);
    for (const MeshManager::Handle handle : handles) {
        EXPECT_TRUE(handle.IsValid());
    }
}

// A mesh that becomes resident on demand has to put its UV1 block on the frame's publish list, not just its geometry.
// The record is what the renderer keys its per-frame descriptor write on; this fixture leaves the renderer uninitialized, so the write itself is out of scope.
TEST_F(MeshUploadBatchGpuTest, ResidencyNotesTheUvTable) {
    const MeshHandle id = registry.Register(MakeQuad(/*with_uv1=*/true));
    registry.RequestGpuResidency(id, mesh_mgr, renderer, vertex_heap, index_heap, uv_heap);

    const auto* info = registry.Get(id);
    ASSERT_NE(info, nullptr);
    ASSERT_TRUE(info->gpu_resident);
    const auto* gpu = mesh_mgr.GetMeshInfo(info->gpu_handle);
    ASSERT_NE(gpu, nullptr);
    ASSERT_TRUE(gpu->has_uv1);
    ASSERT_TRUE(gpu->uv_allocation.IsValid());
    EXPECT_TRUE(registry.WasPublished(TableId::uv, gpu->uv_allocation.buffer_index));
    EXPECT_TRUE(registry.WasPublished(TableId::vertex, gpu->vertex_allocation.buffer_index));
    EXPECT_TRUE(registry.WasPublished(TableId::index, gpu->index_allocation.buffer_index));
}

// A skinned mesh notes its deform table block too, since the deform set binds a different table than the geometry.
TEST_F(MeshUploadBatchGpuTest, ResidencyNotesTheSkinTable) {
    const MeshHandle id = registry.Register(MakeSkinnedMesh());
    registry.RequestGpuResidency(id, mesh_mgr, renderer, vertex_heap, index_heap, uv_heap);

    const auto* info = registry.Get(id);
    ASSERT_NE(info, nullptr);
    ASSERT_TRUE(info->gpu_resident);
    const auto* gpu = mesh_mgr.GetMeshInfo(info->gpu_handle);
    ASSERT_NE(gpu, nullptr);
    ASSERT_EQ(gpu->influence_sets, 1u);
    ASSERT_TRUE(gpu->skin_allocation.IsValid());
    EXPECT_TRUE(registry.WasPublished(TableId::skin, gpu->skin_allocation.buffer_index));
}

// A mesh without UV1 owns no uv block, so it cannot publish one for that table.
TEST_F(MeshUploadBatchGpuTest, ResidencyWithoutUv1TakesNoUvBlock) {
    const MeshHandle id = registry.Register(MakeQuad());
    registry.RequestGpuResidency(id, mesh_mgr, renderer, vertex_heap, index_heap, uv_heap);

    const auto* info = registry.Get(id);
    ASSERT_NE(info, nullptr);
    ASSERT_TRUE(info->gpu_resident);
    const auto* gpu = mesh_mgr.GetMeshInfo(info->gpu_handle);
    ASSERT_NE(gpu, nullptr);
    EXPECT_FALSE(gpu->has_uv1);
    EXPECT_FALSE(gpu->uv_allocation.IsValid());
}

// The publish record is per frame, so the next frame may write the same block again.
TEST_F(MeshUploadBatchGpuTest, EndFrameClearsThePublishRecord) {
    const MeshHandle id = registry.Register(MakeQuad(/*with_uv1=*/true));
    registry.RequestGpuResidency(id, mesh_mgr, renderer, vertex_heap, index_heap, uv_heap);

    const auto* info = registry.Get(id);
    ASSERT_NE(info, nullptr);
    ASSERT_TRUE(info->gpu_resident);
    const auto* gpu = mesh_mgr.GetMeshInfo(info->gpu_handle);
    ASSERT_NE(gpu, nullptr);
    ASSERT_TRUE(gpu->uv_allocation.IsValid());
    ASSERT_TRUE(registry.WasPublished(TableId::uv, gpu->uv_allocation.buffer_index));

    registry.EndFrame(mesh_mgr, VulkanEngine::MeshRenderSystem::EVICTION_TIMEOUT_FRAMES);

    EXPECT_FALSE(registry.WasPublished(TableId::uv, gpu->uv_allocation.buffer_index));
    EXPECT_FALSE(registry.WasPublished(TableId::vertex, gpu->vertex_allocation.buffer_index));
}

} // namespace
