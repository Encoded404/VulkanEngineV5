#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

import std;
import std.compat;

import vulkan;

import test_gpu;

import VulkanBackend.Vulkan.VulkanCapabilities;
import TestSupport.HeadlessVulkanBackend;
import VulkanEngine.ShaderManager;
import VulkanEngine.PipelineFactory;
import VulkanEngine.ShaderRegistration;
import VulkanEngine.SceneRenderer;
import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.Mesh.NormalEncoding;
import VulkanEngine.GpuResources;
import VulkanEngine.ModifierManager;
import VulkanEngine.ModifierId;

namespace {

using VulkanEngine::GpuBoneInfluence;
using VulkanEngine::GpuResources::GpuBuffer;
using VulkanEngine::GpuResources::HeapConfig;
using VulkanEngine::GpuResources::DeviceBufferHeap;
using VulkanEngine::Modifiers::ModifierDesc;
using VulkanEngine::Modifiers::ModifierManager;
using VulkanEngine::SceneRenderer::DrawMode;
using VulkanEngine::SceneRenderer::SceneCapacity;
using VulkanEngine::SceneRenderer::SceneRenderer;
using VulkanEngine::SceneRenderer::SkinEntry;
using VulkanEngine::ShaderSystem::PipelineFactory;
using VulkanEngine::ShaderSystem::ShaderManager;
using VulkanEngine::StandardMeshPipeline::Vertex;

// Device-bound skinning: drives the REAL deform pass (builtin "skinning"
// pipeline, real skin/palette/SkinEntry descriptors) and reads the deformed
// vertices back. The CPU reference uses the same palette convention:
// ComputePalette stores transpose(world * inverseBind), and the shader's
// mul(float4(p, 1), m) computes (world * inverseBind) * p.
class SkinningGpuTest : public ::testing::TestWithParam<DrawMode> {
protected:
    void SetUp() override {
        if (!TestSupport::IsGpuDeviceAvailable()) {
            GTEST_SKIP() << "no Vulkan device available";
        }
        ASSERT_TRUE(backend_.Initialize());
        ASSERT_TRUE(vertex_heap_.Initialize(
            backend_, HeapConfig{.block_size = 4ULL << 20}, "skinning-test-heap"));

        shaders_ = std::make_unique<ShaderManager>(backend_.GetDevice(),
                                                   backend_.GetCapabilities(),
                                                   std::filesystem::temp_directory_path().string());
        ids_.RegisterAll(*shaders_, VKENGINE_ENGINE_SHADER_DIR);
        ASSERT_NE(ids_.deform_comp, VulkanEngine::ShaderSystem::kInvalidShaderId)
            << "the deform shader did not register";

        (void)modifier_mgr_.RegisterBuiltin(ModifierDesc{
            .name = std::string(ModifierManager::kSkinningName),
            .compute_shader = ids_.deform_comp,
            .entry_point = "main",
            .param_size = 0,
            .workgroup_size = 64,
            .writes_normals = true,
            .writes_tangents = true,
            .may_change_bounds = true,
        });

        pipeline_factory_ = std::make_unique<PipelineFactory>(
            backend_.GetDevice(), backend_.GetCapabilities(),
            shaders_->GetPipelineCacheRAII());

        const SceneCapacity capacity{
            .index_count = 1024,
            .vertex_span = 1024,
            .submesh_count = 64,
            .deform_vertex_span = kMaxVertices,
            .palette_matrix_count = 64,
            .modifier_param_bytes = 1024,
        };
        renderer_ = std::make_unique<SceneRenderer>();
        ASSERT_TRUE(renderer_->Initialize(backend_, vertex_heap_, capacity, *shaders_,
                                          *pipeline_factory_, ids_, /*frames_in_flight=*/1,
                                          GetParam(), modifier_mgr_));
        if (renderer_->GetDrawMode() != GetParam()) {
            GTEST_SKIP() << "draw mode " << (GetParam() == DrawMode::MID ? "MID" : "CID")
                         << " is unavailable on this device";
        }
        EXPECT_TRUE(renderer_->IsModifierReady(modifier_mgr_.SkinningId()))
            << "the builtin skinning pipeline was not built";
    }

    void TearDown() override {
        if (!TestSupport::IsGpuDeviceAvailable()) {
            return;
        }
        vertex_buffer_.reset();
        skin_buffer_.reset();
        readback_.reset();
        renderer_->Shutdown();
        renderer_.reset();
        pipeline_factory_.reset();
        shaders_.reset();
        vertex_heap_.Shutdown();
        backend_.Shutdown();
    }

    // Creates the per-test buffers once; the shader reads them raw, so their
    // sizes are fixed at the fixture's capacity.
    void CreateBuffers() {
        if (vertex_buffer_) return;
        vertex_buffer_ = std::make_unique<GpuBuffer>(GpuBuffer::Create(
            backend_, kMaxVertices * sizeof(Vertex), vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent));
        ASSERT_TRUE(vertex_buffer_->IsValid());
        skin_buffer_ = std::make_unique<GpuBuffer>(GpuBuffer::Create(
            backend_, kMaxVertices * sizeof(GpuBoneInfluence),
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent));
        ASSERT_TRUE(skin_buffer_->IsValid());
        readback_ = std::make_unique<GpuBuffer>(GpuBuffer::Create(
            backend_, 2 * kMaxVertices * sizeof(Vertex),
            vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent));
        ASSERT_TRUE(readback_->IsValid());
    }

    // Uploads the test inputs and resolves the frame's descriptors. Call
    // before Run().
    void Configure(std::span<const Vertex> vertices,
                   std::span<const GpuBoneInfluence> influences,
                   std::span<const glm::mat4> palette_matrices,
                   const SkinEntry& entry,
                   const std::uint32_t src_base = 0,
                   const std::uint32_t vertex_count = 0) {
        CreateBuffers();
        vertex_buffer_->Upload(vertices.data(), vertices.size() * sizeof(Vertex));
        skin_buffer_->Upload(influences.data(), influences.size() * sizeof(GpuBoneInfluence));

        renderer_->UpdateVertexBufferArrayElement(
            0, 0, static_cast<vk::Buffer>(*vertex_buffer_->GetBuffer()), vk::WholeSize);
        renderer_->UpdateSkinBufferArrayElement(
            0, 0, static_cast<vk::Buffer>(*skin_buffer_->GetBuffer()), vk::WholeSize);

        auto palettes = renderer_->MapFrameBonePalettes(0);
        ASSERT_GE(palettes.size(), palette_matrices.size());
        for (std::size_t i = 0; i < palette_matrices.size(); ++i) {
            // Column-major storage == glm's memory layout, and the palette holds
            // M = world * inverseBind unchanged (see ComputePalette): the
            // shader's mul(float4(p, 1), m) evaluates M * p.
            std::memcpy(palettes[i].data(), &palette_matrices[i], sizeof(glm::mat4));
        }
        renderer_->UnmapFrameBonePalettes(0);

        auto& entries = renderer_->GetFrameSkinEntries(0);
        ASSERT_TRUE(entries.EnsureCapacity(1));
        auto* stored = static_cast<SkinEntry*>(entries.Get(0));
        ASSERT_NE(stored, nullptr);
        *stored = entry;

        VulkanEngine::SceneRenderer::ModifierDispatch dispatch{};
        dispatch.submesh_index = 0;
        dispatch.stage = 0;
        dispatch.modifier = modifier_mgr_.SkinningId();
        dispatch.vertex_count =
            vertex_count != 0 ? vertex_count : static_cast<std::uint32_t>(vertices.size());
        dispatch.src_slot = 0;
        dispatch.src_base = src_base;
        dispatch.workgroup_size = 64;
        renderer_->SetModifierDispatches(0, std::span<const VulkanEngine::SceneRenderer::ModifierDispatch>(
            &dispatch, 1));
        renderer_->SetHasActiveDeform(true);

        VulkanEngine::ComponentRegistry registry;
        renderer_->PrepareCompute({}, registry, view_, projection_, 1280, 720, 0);
    }

    // Runs the deform pass and reads back the vertices of the final region.
    [[nodiscard]] std::vector<Vertex> Run() {
        const vk::Buffer deform = renderer_->GetFrameDeformVertices(0);
        EXPECT_TRUE(deform) << "no deform vertex buffer";
        const vk::DeviceSize bytes = 2 * kMaxVertices * sizeof(Vertex);

        vk::raii::CommandBuffer& cmd = backend_.GetCommandBuffer(0);
        cmd.reset({});
        cmd.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        renderer_->DispatchDeform(cmd, 0);
        const vk::BufferMemoryBarrier to_copy(
            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead,
            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, deform, 0, vk::WholeSize);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, to_copy, {});
        cmd.copyBuffer(deform, static_cast<vk::Buffer>(*readback_->GetBuffer()),
                       vk::BufferCopy(0, 0, bytes));
        const vk::BufferMemoryBarrier to_host(
            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead,
            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
            static_cast<vk::Buffer>(*readback_->GetBuffer()), 0, bytes);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eHost, {}, {}, to_host, {});
        cmd.end();
        backend_.GetGraphicsQueue().submit(vk::SubmitInfo(0, nullptr, nullptr, 1, &*cmd), {});
        backend_.GetGraphicsQueue().waitIdle();

        std::vector<Vertex> out(2 * kMaxVertices);
        const void* mapped = readback_->Map(0, bytes);
        std::memcpy(out.data(), mapped, static_cast<std::size_t>(bytes));
        readback_->Unmap();
        return out;
    }

    // ── Test-side reference decode of the packed tangent frame ──
    // Needed only because the C++ side ships the packer, not the decoder.
    [[nodiscard]] static glm::vec3 DecodeOctahedral(const glm::vec2& f) {
        glm::vec3 n{f.x, f.y, 1.0f - std::abs(f.x) - std::abs(f.y)};
        const float t = std::max(-n.z, 0.0f);
        n.x += n.x >= 0.0f ? -t : t;
        n.y += n.y >= 0.0f ? -t : t;
        return glm::normalize(n);
    }

    [[nodiscard]] static glm::vec3 DecodeNormal(const std::uint32_t packed) {
        constexpr float kMax = 1023.0f;
        const glm::vec2 f{
            static_cast<float>(packed & 0x3FFu) / kMax * 2.0f - 1.0f,
            static_cast<float>((packed >> 10) & 0x3FFu) / kMax * 2.0f - 1.0f,
        };
        return DecodeOctahedral(f);
    }

    static constexpr std::uint32_t kMaxVertices = 8;

    // A perspective camera so the deform pass has a real view/projection; the
    // deform stage itself does not use them, but PrepareCompute sizes projInfo
    // from them.
    glm::mat4 view_{1.0f};
    glm::mat4 projection_{glm::perspective(glm::radians(60.0f), 1280.0f / 720.0f, 0.1f, 100.0f)};

    TestSupport::HeadlessVulkanBackend backend_{};
    DeviceBufferHeap vertex_heap_{};
    ModifierManager modifier_mgr_{};
    std::unique_ptr<ShaderManager> shaders_;
    std::unique_ptr<PipelineFactory> pipeline_factory_;
    VulkanEngine::EngineShaderIds ids_{};
    std::unique_ptr<SceneRenderer> renderer_;
    std::unique_ptr<GpuBuffer> vertex_buffer_;
    std::unique_ptr<GpuBuffer> skin_buffer_;
    std::unique_ptr<GpuBuffer> readback_;
};

// One vertex at a known position with a valid tangent frame.
[[nodiscard]] Vertex MakeVertex(const float x, const float y, const float z) {
    Vertex v{};
    v.px = x;
    v.py = y;
    v.pz = z;
    v.packedTBN = VulkanEngine::NormalEncoding::PackTBN(
        VulkanEngine::MeshVertexVec3{0.0f, 0.0f, 1.0f},
        VulkanEngine::MeshVertexVec3{1.0f, 0.0f, 0.0f}, 1.0f);
    v.u = 0.25f;
    v.v = 0.75f;
    return v;
}

[[nodiscard]] GpuBoneInfluence MakeInfluence(const std::uint32_t j0, const std::uint32_t j1,
                                             const std::uint8_t w0, const std::uint8_t w1) {
    GpuBoneInfluence b{};
    b.joints01 = j0 | (j1 << 16);
    b.weights = static_cast<std::uint32_t>(w0) | (static_cast<std::uint32_t>(w1) << 8);
    return b;
}

// A skin entry that gathers would produce for a single-set skinned submesh
// with one allocated deform region (expand's output for an even stack).
[[nodiscard]] SkinEntry MakeEntry(const std::uint32_t vertex_count, const bool active) {
    SkinEntry e{};
    e.palette_base = 0;
    e.skin_slot = 0;
    e.skin_bias = 0;
    e.extra_slot = VulkanEngine::SceneRenderer::kNoSkinSlot;
    e.extra_bias = 0;
    e.influence_sets = 1;
    e.flags = VulkanEngine::SceneRenderer::kSkinned |
              (active ? (VulkanEngine::SceneRenderer::kDeformActive |
                         VulkanEngine::SceneRenderer::kDeformNoOccluder |
                         VulkanEngine::SceneRenderer::kDeformNoCullTest)
                      : 0u);
    e.deform_base = 0;
    (void)vertex_count;
    return e;
}

TEST_P(SkinningGpuTest, IdentityPaletteReproducesTheSourceVertex) {
    const std::vector<Vertex> vertices{MakeVertex(1.0f, 2.0f, 3.0f), MakeVertex(-1.5f, 0.5f, 0.0f)};
    const std::vector<GpuBoneInfluence> influences{
        MakeInfluence(0, 1, 255, 0), MakeInfluence(0, 1, 255, 0)};
    const std::vector<glm::mat4> palettes{glm::mat4(1.0f)};

    Configure(vertices, influences, palettes, MakeEntry(2, true));
    const auto out = Run();

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        EXPECT_NEAR(out[i].px, vertices[i].px, 1e-5f);
        EXPECT_NEAR(out[i].py, vertices[i].py, 1e-5f);
        EXPECT_NEAR(out[i].pz, vertices[i].pz, 1e-5f);
        EXPECT_NEAR(out[i].u, vertices[i].u, 1e-6f);
        EXPECT_NEAR(out[i].v, vertices[i].v, 1e-6f);

        const glm::vec3 expected_normal = DecodeNormal(vertices[i].packedTBN);
        const glm::vec3 actual_normal = DecodeNormal(out[i].packedTBN);
        EXPECT_NEAR(actual_normal.x, expected_normal.x, 1e-3f);
        EXPECT_NEAR(actual_normal.y, expected_normal.y, 1e-3f);
        EXPECT_NEAR(actual_normal.z, expected_normal.z, 1e-3f);
        // Handedness is copied through, not recomputed.
        EXPECT_EQ((out[i].packedTBN >> 31), (vertices[i].packedTBN >> 31));
    }
}

TEST_P(SkinningGpuTest, TwoJointWeightsBlendPositions) {
    const std::vector<Vertex> vertices{MakeVertex(1.0f, 0.0f, 0.0f)};
    // 128 + 127 == 255: the quantized weights the importer would store.
    const std::vector<GpuBoneInfluence> influences{MakeInfluence(0, 1, 128, 127)};
    const glm::mat4 joint0(1.0f);
    const glm::mat4 joint1 = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f));
    const std::vector<glm::mat4> palettes{joint0, joint1};

    Configure(vertices, influences, palettes, MakeEntry(1, true));
    const auto out = Run();

    const glm::vec3 expected =
        (128.0f / 255.0f) * glm::vec3(joint0 * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f)) +
        (127.0f / 255.0f) * glm::vec3(joint1 * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(out[0].px, expected.x, 1e-5f);
    EXPECT_NEAR(out[0].py, expected.y, 1e-5f);
    EXPECT_NEAR(out[0].pz, expected.z, 1e-5f);
}

TEST_P(SkinningGpuTest, ARotatedJointRotatesTheNormal) {
    const std::vector<Vertex> vertices{MakeVertex(1.0f, 0.0f, 0.0f)};
    const std::vector<GpuBoneInfluence> influences{MakeInfluence(0, 0, 255, 0)};
    const glm::mat4 rotation =
        glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const std::vector<glm::mat4> palettes{rotation};

    Configure(vertices, influences, palettes, MakeEntry(1, true));
    const auto out = Run();

    // The authored normal is +Z: a 90 degree rotation about Z must leave it
    // unchanged, while the position sweeps from +X to +Y.
    const glm::vec3 expected_pos = glm::vec3(rotation * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    EXPECT_NEAR(out[0].px, expected_pos.x, 1e-5f);
    EXPECT_NEAR(out[0].py, expected_pos.y, 1e-5f);
    EXPECT_NEAR(out[0].pz, expected_pos.z, 1e-5f);

    const glm::vec3 normal = DecodeNormal(out[0].packedTBN);
    EXPECT_NEAR(normal.x, 0.0f, 1e-3f);
    EXPECT_NEAR(normal.y, 0.0f, 1e-3f);
    EXPECT_NEAR(normal.z, 1.0f, 1e-3f);
}

TEST_P(SkinningGpuTest, InfluenceLookupIncludesTheSubmeshWindowBase) {
    // The submesh's tight window starts at source element 2, so its first vertex
    // is source vertex 2 and its influence records start at record 2. Records 0
    // and 1 are decoys: reading them instead would move the vertex by +5.
    const std::vector<Vertex> vertices{
        MakeVertex(0.0f, 0.0f, 0.0f), MakeVertex(0.0f, 0.0f, 0.0f),
        MakeVertex(1.0f, 0.0f, 0.0f), MakeVertex(0.0f, 0.0f, 0.0f)};
    const std::vector<GpuBoneInfluence> influences{
        MakeInfluence(1, 0, 255, 0), MakeInfluence(1, 0, 255, 0),
        MakeInfluence(0, 0, 255, 0), MakeInfluence(0, 0, 255, 0)};
    const std::vector<glm::mat4> palettes{
        glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f))};

    // gather: skin_bias = skin_region_base (0) - plan.base_vertex (0), and the
    // dispatch's src_base = base_vertex + vertex_window_base = 2.
    SkinEntry entry = MakeEntry(1, true);
    entry.skin_bias = 0;
    Configure(vertices, influences, palettes, entry, /*src_base=*/2, /*vertex_count=*/1);

    const auto out = Run();
    EXPECT_NEAR(out[0].px, 1.0f, 1e-5f)
        << "the influence lookup dropped the submesh window base and read a "
           "different vertex's joints/weights";
}

TEST_P(SkinningGpuTest, ARejectedRegionLeavesTheOutputUntouched) {
    const std::vector<Vertex> vertices{MakeVertex(1.0f, 0.0f, 0.0f)};
    const std::vector<GpuBoneInfluence> influences{MakeInfluence(0, 0, 255, 0)};
    const glm::mat4 moved = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f));
    const std::vector<glm::mat4> palettes{moved};

    // First run writes the deformed region.
    Configure(vertices, influences, palettes, MakeEntry(1, true));
    const auto first = Run();
    ASSERT_NEAR(first[0].px, 6.0f, 1e-5f);

    // Same dispatch, but expand cleared kDeformActive (its region budget was
    // too small). The shader must return before writing, so the region still
    // holds the first run's result.
    Configure(vertices, influences, palettes, MakeEntry(1, false));
    const auto second = Run();
    EXPECT_NEAR(second[0].px, first[0].px, 1e-6f);
    EXPECT_NEAR(second[0].py, first[0].py, 1e-6f);
    EXPECT_NEAR(second[0].pz, first[0].pz, 1e-6f);
}

INSTANTIATE_TEST_SUITE_P(BothDrawModes, SkinningGpuTest,
                         ::testing::Values(DrawMode::CID, DrawMode::MID),
                         [](const ::testing::TestParamInfo<DrawMode>& info) {
                             return info.param == DrawMode::MID ? "MID" : "CID";
                         });

}  // namespace
