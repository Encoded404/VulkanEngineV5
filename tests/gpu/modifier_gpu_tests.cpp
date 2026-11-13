#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

import std;
import std.compat;

import vulkan;

import test_gpu;

import ShaderReflection;

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
using VulkanEngine::Modifiers::ModifierId;
using VulkanEngine::Modifiers::ModifierManager;
using VulkanEngine::SceneRenderer::DrawMode;
using VulkanEngine::SceneRenderer::ModifierDispatch;
using VulkanEngine::SceneRenderer::SceneCapacity;
using VulkanEngine::SceneRenderer::SceneRenderer;
using VulkanEngine::SceneRenderer::SkinEntry;
using VulkanEngine::ShaderSystem::PipelineFactory;
using VulkanEngine::ShaderSystem::ShaderId;
using VulkanEngine::ShaderSystem::ShaderManager;
using VulkanEngine::StandardMeshPipeline::Vertex;

// Device-bound application modifiers: registers a test kernel through
// ModifierManager exactly as an app would, then drives the real deform pass
// (real descriptor sets, real pipeline built by SyncModifierPipelines, the
// frame's CPU dispatch list) and reads the deformed vertices back.
//
// What this covers that the device-free dispatch tests cannot: parameter blocks
// actually reaching the shader, the A/B region parity as the shader computes it,
// per-submesh regions, the intra-pass barrier between stages, and the retire
// path that stops dispatching an unregistered modifier.
class ModifierGpuTest : public ::testing::TestWithParam<DrawMode> {
protected:
    // One submesh's deform region (its vertex window). The frame's regions are
    // laid out contiguously at this stride: the two-submesh tests put the second
    // submesh at kRegionSpan.
    static constexpr std::uint32_t kRegionSpan = 8;
    // SceneCapacity::deform_vertex_span is the *frame* total (the sum of every
    // deforming submesh's span), and the buffer holds two regions per element of
    // it. Two submeshes of kRegionSpan therefore need a 2*kRegionSpan frame span
    // and a 4*kRegionSpan buffer; expand would reject the second submesh
    // otherwise.
    static constexpr std::uint32_t kFrameSpan = 2 * kRegionSpan;
    static constexpr std::uint32_t kVertices = 8;
    static constexpr std::uint32_t kDeformVertices = 2 * kFrameSpan;
    // Parameter bytes the probe declares: one float4.
    static constexpr std::uint32_t kProbeParamBytes = 16;
    static constexpr std::uint32_t kParamBufferBytes = 256;

    void SetUp() override {
        if (!TestSupport::IsGpuDeviceAvailable()) {
            GTEST_SKIP() << "no Vulkan device available";
        }
        ASSERT_TRUE(backend_.Initialize());
        ASSERT_TRUE(vertex_heap_.Initialize(
            backend_, HeapConfig{.block_size = 4ULL << 20}, "modifier-test-heap"));

        shaders_ = std::make_unique<ShaderManager>(backend_.GetDevice(),
                                                   backend_.GetCapabilities(),
                                                   std::filesystem::temp_directory_path().string());
        ids_.RegisterAll(*shaders_, VKENGINE_ENGINE_SHADER_DIR);
        ASSERT_NE(ids_.deform_comp, VulkanEngine::ShaderSystem::kInvalidShaderId)
            << "the deform shader did not register";

        app_shader_ = shaders_->RegisterManual(
            std::format("{}/test_modifier_apply.spv", VKENGINE_TEST_SHADER_DIR), "",
            ShaderStage::eCompute);
        ASSERT_NE(app_shader_, VulkanEngine::ShaderSystem::kInvalidShaderId)
            << "the app-modifier probe did not register";

        // The builtin skinning stage, exactly as GameEngine registers it.
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
        // An application modifier, registered through the same entry point an app
        // uses (ModifierManager::Register, never RegisterBuiltin).
        app_id_ = modifier_mgr_.Register(ModifierDesc{
            .name = "probe_apply",
            .compute_shader = app_shader_,
            .entry_point = "main",
            .param_size = kProbeParamBytes,
            .param_alignment = 16,
            .workgroup_size = 64,
        });
        ASSERT_TRUE(app_id_.IsValid()) << "the app modifier was rejected";

        pipeline_factory_ = std::make_unique<PipelineFactory>(
            backend_.GetDevice(), backend_.GetCapabilities(),
            shaders_->GetPipelineCacheRAII());

        const SceneCapacity capacity{
            .index_count = 1024,
            .vertex_span = 1024,
            .submesh_count = 8,
            .deform_vertex_span = kFrameSpan,
            .palette_matrix_count = 16,
            .modifier_param_bytes = kParamBufferBytes,
        };
        renderer_ = std::make_unique<SceneRenderer>();
        ASSERT_TRUE(renderer_->Initialize(backend_, vertex_heap_, capacity, *shaders_,
                                          *pipeline_factory_, ids_, /*frames_in_flight=*/1,
                                          GetParam(), modifier_mgr_));
        // A headless bootstrap has no swapchain extent; PrepareCompute writes the
        // Hi-Z sampler descriptors, so create them at the test's render extent.
        renderer_->EnsureRenderExtent(1280, 720);
        if (renderer_->GetDrawMode() != GetParam()) {
            GTEST_SKIP() << "draw mode " << (GetParam() == DrawMode::MID ? "MID" : "CID")
                         << " is unavailable on this device";
        }
        // SyncModifierPipelines ran inside Initialize: both entries must have a
        // live pipeline before a single dispatch can be recorded.
        ASSERT_TRUE(renderer_->IsModifierReady(modifier_mgr_.SkinningId()))
            << "the builtin skinning pipeline was not built";
        ASSERT_TRUE(renderer_->IsModifierReady(app_id_))
            << "the application modifier pipeline was not built";
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

    void CreateBuffers() {
        if (vertex_buffer_) return;
        vertex_buffer_ = std::make_unique<GpuBuffer>(GpuBuffer::Create(
            backend_, kVertices * sizeof(Vertex), vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent));
        ASSERT_TRUE(vertex_buffer_->IsValid());
        // Always bound: the probe's common header declares the influence tables
        // even though an unskinned modifier never reads them.
        skin_buffer_ = std::make_unique<GpuBuffer>(GpuBuffer::Create(
            backend_, kVertices * sizeof(GpuBoneInfluence),
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent));
        ASSERT_TRUE(skin_buffer_->IsValid());
        readback_ = std::make_unique<GpuBuffer>(GpuBuffer::Create(
            backend_, kDeformVertices * sizeof(Vertex),
            vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent));
        ASSERT_TRUE(readback_->IsValid());
    }

    // Uploads the inputs for one dispatch list and resolves the frame's
    // descriptors. `params` is the flat parameter buffer: a stage reads the
    // block at its own `param_offset`.
    void Configure(std::span<const Vertex> vertices,
                   std::span<const SkinEntry> entries,
                   std::span<const ModifierDispatch> dispatches,
                   std::span<const std::byte> params = {},
                   std::span<const glm::mat4> palettes = {},
                   std::span<const GpuBoneInfluence> influences = {}) {
        CreateBuffers();
        const std::size_t vertex_count = std::min<std::size_t>(vertices.size(), kVertices);
        vertex_buffer_->Upload(vertices.data(), vertex_count * sizeof(Vertex));
        if (!influences.empty()) {
            skin_buffer_->Upload(influences.data(),
                                 std::min<std::size_t>(influences.size(), kVertices) *
                                     sizeof(GpuBoneInfluence));
        } else {
            // A zero record: an unskinned submesh's influence stream is never
            // read, but the descriptor must still name a live buffer.
            const std::array<GpuBoneInfluence, kVertices> zero{};
            skin_buffer_->Upload(zero.data(), sizeof(zero));
        }

        renderer_->UpdateVertexBufferArrayElement(
            0, 0, static_cast<vk::Buffer>(*vertex_buffer_->GetBuffer()), vk::WholeSize);
        renderer_->UpdateSkinBufferArrayElement(
            0, 0, static_cast<vk::Buffer>(*skin_buffer_->GetBuffer()), vk::WholeSize);

        const auto palette_span = renderer_->MapFrameBonePalettes(0);
        ASSERT_FALSE(palette_span.empty());
        for (std::size_t i = 0; i < palettes.size() && i < palette_span.size(); ++i) {
            // Column-major storage == glm's layout; see ComputePalette for the
            // mul(float4(p, 1), m) convention the shaders use.
            std::memcpy(palette_span[i].data(), &palettes[i], sizeof(glm::mat4));
        }
        renderer_->UnmapFrameBonePalettes(0);

        const auto param_span = renderer_->MapFrameModifierParams(0);
        ASSERT_FALSE(param_span.empty());
        std::ranges::fill(param_span, std::byte{0});
        ASSERT_LE(params.size(), param_span.size());
        if (!params.empty()) {
            std::memcpy(param_span.data(), params.data(), params.size());
        }
        renderer_->UnmapFrameModifierParams(0);

        auto& skin_entries = renderer_->GetFrameSkinEntries(0);
        ASSERT_TRUE(skin_entries.EnsureCapacity(
            static_cast<std::uint32_t>(std::max<std::size_t>(entries.size(), 1))));
        for (std::size_t i = 0; i < entries.size(); ++i) {
            auto* stored = static_cast<SkinEntry*>(skin_entries.Get(static_cast<std::uint32_t>(i)));
            ASSERT_NE(stored, nullptr);
            *stored = entries[i];
        }

        renderer_->SetModifierDispatches(0, dispatches);
        renderer_->SetHasActiveDeform(!dispatches.empty());

        VulkanEngine::ComponentRegistry registry;
        renderer_->PrepareCompute({}, registry, view, projection, 1280, 720, 0);
    }

    // Runs the deform pass and reads the whole two-region buffer back.
    [[nodiscard]] std::vector<Vertex> Run() {
        const vk::Buffer deform = renderer_->GetFrameDeformVertices(0);
        EXPECT_TRUE(deform) << "no deform vertex buffer";
        const vk::DeviceSize bytes = kDeformVertices * sizeof(Vertex);

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

        std::vector<Vertex> out(kDeformVertices);
        const void* mapped = readback_->Map(0, bytes);
        std::memcpy(out.data(), mapped, static_cast<std::size_t>(bytes));
        readback_->Unmap();
        return out;
    }

    // The source vertex table, as the GPU sees it. A modifier may not write it.
    [[nodiscard]] std::vector<Vertex> ReadSource(const std::uint32_t count) {
        const void* mapped = vertex_buffer_->Map(0, count * sizeof(Vertex));
        std::vector<Vertex> out(count);
        std::memcpy(out.data(), mapped, count * sizeof(Vertex));
        vertex_buffer_->Unmap();
        return out;
    }

    // A camera so PrepareCompute has a real view/projection to size projInfo from;
    // the deform stage itself never reads it.
    glm::mat4 view{1.0f};
    glm::mat4 projection{glm::perspective(glm::radians(60.0f), 1280.0f / 720.0f, 0.1f, 100.0f)};

    TestSupport::HeadlessVulkanBackend backend_{};
    DeviceBufferHeap vertex_heap_{};
    ModifierManager modifier_mgr_{};
    std::unique_ptr<ShaderManager> shaders_;
    std::unique_ptr<PipelineFactory> pipeline_factory_;
    VulkanEngine::EngineShaderIds ids_{};
    ShaderId app_shader_ = VulkanEngine::ShaderSystem::kInvalidShaderId;
    ModifierId app_id_{};
    std::unique_ptr<SceneRenderer> renderer_;
    std::unique_ptr<GpuBuffer> vertex_buffer_;
    std::unique_ptr<GpuBuffer> skin_buffer_;
    std::unique_ptr<GpuBuffer> readback_;
};

// The probe's parameter block: one float4 `op`, 16 bytes, matching
// test_modifier_apply.cs.slang and ModifierDesc::param_size.
struct ProbeParams {
    std::array<float, 4> op{};
};
static_assert(sizeof(ProbeParams) == 16, "the probe's parameter block is one float4");

[[nodiscard]] ProbeParams Translate(const float x, const float y, const float z) {
    return ProbeParams{{0.0f, x, y, z}};
}
[[nodiscard]] ProbeParams ScaleBy(const float s) {
    return ProbeParams{{1.0f, s, 0.0f, 0.0f}};
}
[[nodiscard]] ProbeParams ConstantAt(const float x, const float y, const float z) {
    return ProbeParams{{2.0f, x, y, z}};
}

// One vertex at a known position with a valid tangent frame. Position i is
// (i, 2i, 3i), so every stage's arithmetic is distinguishable from every other.
[[nodiscard]] Vertex MakeVertex(const std::uint32_t index) {
    Vertex v{};
    v.px = static_cast<float>(index);
    v.py = static_cast<float>(index) * 2.0f;
    v.pz = static_cast<float>(index) * 3.0f;
    v.packedTBN = VulkanEngine::NormalEncoding::PackTBN(
        VulkanEngine::MeshVertexVec3{0.0f, 0.0f, 1.0f},
        VulkanEngine::MeshVertexVec3{1.0f, 0.0f, 0.0f}, 1.0f);
    v.u = 0.25f;
    v.v = 0.75f;
    return v;
}

[[nodiscard]] std::vector<Vertex> MakeVertices(const std::uint32_t count = 8) {
    std::vector<Vertex> vertices(count);
    for (std::uint32_t i = 0; i < count; ++i) vertices[i] = MakeVertex(i);
    return vertices;
}

// Copies `p` into the flat parameter buffer at `byte_offset`. Offsets are the
// ones gather would hand out: the plan's own alignment first, then 16 B per
// block.
void PutParams(std::span<std::byte> buffer, const std::uint32_t byte_offset,
               const ProbeParams& p) {
    ASSERT_LE(byte_offset + sizeof(p), buffer.size());
    std::memcpy(buffer.data() + byte_offset, &p, sizeof(p));
}

// An unskinned submesh with a valid deform region: gather sets the cull bits and
// expand allocates A at `deform_base`.
[[nodiscard]] SkinEntry MakeUnskinnedEntry(const std::uint32_t deform_base) {
    SkinEntry e{};
    e.palette_base = 0;
    e.skin_slot = 0;
    e.skin_bias = 0;
    e.extra_slot = VulkanEngine::SceneRenderer::kNoSkinSlot;
    e.extra_bias = 0;
    e.influence_sets = 0;
    e.flags = VulkanEngine::SceneRenderer::kDeformActive |
              VulkanEngine::SceneRenderer::kDeformNoOccluder |
              VulkanEngine::SceneRenderer::kDeformNoCullTest;
    e.deform_base = deform_base;
    return e;
}

// An entry whose region expand rejected: kDeformActive cleared and deform_base
// left at the 0 gather wrote, because expand only assigns a base when it
// actually allocates.
[[nodiscard]] SkinEntry MakeRejectedEntry(const std::uint32_t deform_base) {
    SkinEntry e = MakeUnskinnedEntry(deform_base);
    e.flags &= ~VulkanEngine::SceneRenderer::kDeformActive;
    return e;
}

// The same entry for a skinned submesh: skinning can read the tables.
[[nodiscard]] SkinEntry MakeSkinnedEntry(const std::uint32_t deform_base) {
    SkinEntry e = MakeUnskinnedEntry(deform_base);
    e.flags |= VulkanEngine::SceneRenderer::kSkinned;
    e.influence_sets = 1;
    return e;
}

// One dispatch as gather emits it. `param_size` is 0 for the builtin skinning
// stage, which reads no parameter block.
[[nodiscard]] ModifierDispatch MakeDispatch(const std::uint32_t submesh_index,
                                            const std::uint32_t stage,
                                            const ModifierId id,
                                            const std::uint32_t vertex_count,
                                            const std::uint32_t src_base = 0,
                                            const std::uint32_t param_offset = 0,
                                            const std::uint32_t param_size = 16) {
    ModifierDispatch d{};
    d.submesh_index = submesh_index;
    d.stage = stage;
    d.modifier = id;
    d.vertex_count = vertex_count;
    d.src_slot = 0;
    d.src_base = src_base;
    d.param_offset = param_offset;
    d.param_size = param_size;
    d.workgroup_size = 64;
    return d;
}

TEST_P(ModifierGpuTest, OneAppModifierWritesRegionAAndPreservesTheFrame) {
    const auto vertices = MakeVertices();
    const SkinEntry entry = MakeUnskinnedEntry(0);

    std::array<std::byte, 256> params{};
    PutParams(params, 0, Translate(10.0f, 20.0f, 30.0f));

    const ModifierDispatch dispatch =
        MakeDispatch(0, 0, app_id_, static_cast<std::uint32_t>(vertices.size()));

    Configure(vertices, std::span<const SkinEntry>(&entry, 1), std::span(&dispatch, 1), params);
    const auto out = Run();

    // An unskinned stack is one stage: stage 0 is even, so it writes region A.
    for (std::uint32_t i = 0; i < vertices.size(); ++i) {
        EXPECT_NEAR(out[i].px, static_cast<float>(i) + 10.0f, 1e-5f) << "element " << i;
        EXPECT_NEAR(out[i].py, static_cast<float>(i) * 2.0f + 20.0f, 1e-5f) << "element " << i;
        EXPECT_NEAR(out[i].pz, static_cast<float>(i) * 3.0f + 30.0f, 1e-5f) << "element " << i;
        // UVs and the tangent frame are carried through untouched.
        EXPECT_FLOAT_EQ(out[i].u, vertices[i].u);
        EXPECT_FLOAT_EQ(out[i].v, vertices[i].v);
        EXPECT_EQ(out[i].packedTBN, vertices[i].packedTBN);
    }

    // The app modifier is a 1:1 transform of the vertex table, never a write to
    // it: a second frame must still start from the authored positions.
    const auto source = ReadSource(static_cast<std::uint32_t>(vertices.size()));
    for (std::uint32_t i = 0; i < vertices.size(); ++i) {
        EXPECT_FLOAT_EQ(source[i].px, vertices[i].px) << "the modifier wrote the source table";
        EXPECT_FLOAT_EQ(source[i].py, vertices[i].py);
        EXPECT_FLOAT_EQ(source[i].pz, vertices[i].pz);
    }
}

TEST_P(ModifierGpuTest, TwoStagesComposeAndEndInRegionB) {
    const auto vertices = MakeVertices();
    const SkinEntry entry = MakeUnskinnedEntry(0);

    std::array<std::byte, 256> params{};
    PutParams(params, 0, Translate(10.0f, 20.0f, 30.0f));
    PutParams(params, 16, ScaleBy(2.0f));

    const std::array<ModifierDispatch, 2> dispatches{
        MakeDispatch(0, 0, app_id_, static_cast<std::uint32_t>(vertices.size()), 0, 0),
        MakeDispatch(0, 1, app_id_, static_cast<std::uint32_t>(vertices.size()), 0, 16),
    };

    Configure(vertices, std::span<const SkinEntry>(&entry, 1), dispatches, params);
    const auto out = Run();

    // Stage 0 wrote A (translate) and stage 1 read it back and wrote B (scale).
    // The composition is therefore (p + t) * 2, which only holds if stage 1 saw
    // stage 0's writes -- the reason DispatchDeform barriers between stages.
    // Region B is the final one: an even stack ends there (kFinalRegionB).
    for (std::uint32_t i = 0; i < vertices.size(); ++i) {
        const float x = static_cast<float>(i);
        EXPECT_NEAR(out[kFrameSpan + i].px, (x + 10.0f) * 2.0f, 1e-5f) << "element " << i;
        EXPECT_NEAR(out[kFrameSpan + i].py, (x * 2.0f + 20.0f) * 2.0f, 1e-5f) << "element " << i;
        EXPECT_NEAR(out[kFrameSpan + i].pz, (x * 3.0f + 30.0f) * 2.0f, 1e-5f) << "element " << i;
        // Region A still holds the intermediate result of stage 0.
        EXPECT_NEAR(out[i].px, x + 10.0f, 1e-5f) << "element " << i;
        EXPECT_NEAR(out[i].py, x * 2.0f + 20.0f, 1e-5f) << "element " << i;
        EXPECT_NEAR(out[i].pz, x * 3.0f + 30.0f, 1e-5f) << "element " << i;
    }
    // Explicitly pin where the final region is: an even stack ends in B.
    EXPECT_NEAR(out[kFrameSpan + 3].px, (3.0f + 10.0f) * 2.0f, 1e-5f);
}

TEST_P(ModifierGpuTest, StackOrderIsDispatchOrder) {
    // The same two operations in the opposite order: (p * 2) + t, which is a
    // different result from TwoStagesComposeAndEndInRegionB. A dispatch loop that
    // reordered or deduplicated stages could not produce both.
    const auto vertices = MakeVertices();
    const SkinEntry entry = MakeUnskinnedEntry(0);

    std::array<std::byte, 256> params{};
    PutParams(params, 0, ScaleBy(2.0f));
    PutParams(params, 16, Translate(10.0f, 20.0f, 30.0f));

    const std::array<ModifierDispatch, 2> dispatches{
        MakeDispatch(0, 0, app_id_, static_cast<std::uint32_t>(vertices.size()), 0, 0),
        MakeDispatch(0, 1, app_id_, static_cast<std::uint32_t>(vertices.size()), 0, 16),
    };

    Configure(vertices, std::span<const SkinEntry>(&entry, 1), dispatches, params);
    const auto out = Run();

    for (std::uint32_t i = 0; i < vertices.size(); ++i) {
        const float x = static_cast<float>(i);
        EXPECT_NEAR(out[kFrameSpan + i].px, x * 2.0f + 10.0f, 1e-5f) << "element " << i;
        EXPECT_NEAR(out[kFrameSpan + i].py, x * 4.0f + 20.0f, 1e-5f) << "element " << i;
        EXPECT_NEAR(out[kFrameSpan + i].pz, x * 6.0f + 30.0f, 1e-5f) << "element " << i;
    }
}

TEST_P(ModifierGpuTest, EveryStageCountReadsBackTheParityRegion) {
    const auto vertices = MakeVertices();
    const SkinEntry entry = MakeUnskinnedEntry(0);

    // Every stack below translates by +1 per stage, so the final position is
    // p + stage_count. Each stage gets its own parameter block, like gather
    // laying out a plan's stages contiguously.
    for (std::uint32_t stage_count = 1; stage_count <= 4; ++stage_count) {
        std::array<std::byte, 256> params{};
        std::vector<ModifierDispatch> dispatches;
        for (std::uint32_t s = 0; s < stage_count; ++s) {
            PutParams(params, s * 16, Translate(1.0f, 1.0f, 1.0f));
            dispatches.push_back(MakeDispatch(0, s, app_id_,
                                              static_cast<std::uint32_t>(vertices.size()), 0,
                                              s * 16));
        }

        Configure(vertices, std::span<const SkinEntry>(&entry, 1), dispatches, params);
        const auto out = Run();

        // Odd stacks end in A, even stacks in B (expand's kFinalRegionB).
        const std::uint32_t final_base = (stage_count % 2 == 0) ? kFrameSpan : 0u;
        for (std::uint32_t i = 0; i < vertices.size(); ++i) {
            const float expected = static_cast<float>(i) + static_cast<float>(stage_count);
            EXPECT_NEAR(out[final_base + i].px, expected, 1e-4f)
                << "stage_count=" << stage_count << " element " << i;
        }
        // The other region holds the penultimate result (a single stage leaves
        // it untouched, so only stacks of two or more have a defined value
        // there).
        if (stage_count >= 2) {
            const std::uint32_t other_base = final_base == 0 ? kFrameSpan : 0u;
            EXPECT_NEAR(out[other_base + 0].px, static_cast<float>(stage_count) - 1.0f, 1e-4f)
                << "stage_count=" << stage_count;
        }
    }
}

TEST_P(ModifierGpuTest, EachSubmeshHasItsOwnRegionAndParameterBlock) {
    const auto vertices = MakeVertices();
    // Two unskinned submeshes sharing the same source vertices: gather gives each
    // its own deform region (T apart) and its own parameter block.
    const std::array<SkinEntry, 2> entries{MakeUnskinnedEntry(0),
                                           MakeUnskinnedEntry(kRegionSpan)};

    std::array<std::byte, 256> params{};
    PutParams(params, 0, Translate(1.0f, 0.0f, 0.0f));
    PutParams(params, 32, Translate(0.0f, 100.0f, 0.0f));

    const std::array<ModifierDispatch, 2> dispatches{
        MakeDispatch(0, 0, app_id_, static_cast<std::uint32_t>(vertices.size()), 0, 0),
        MakeDispatch(1, 0, app_id_, static_cast<std::uint32_t>(vertices.size()), 0, 32),
    };

    Configure(vertices, entries, dispatches, params);
    const auto out = Run();

    // Submesh 0 wrote A at 0, submesh 1 wrote A at kRegionSpan. Both are single
    // stages, so both are in their own A.
    for (std::uint32_t i = 0; i < vertices.size(); ++i) {
        EXPECT_NEAR(out[i].px, static_cast<float>(i) + 1.0f, 1e-5f);
        EXPECT_NEAR(out[i].py, static_cast<float>(i) * 2.0f, 1e-5f);
        EXPECT_NEAR(out[kRegionSpan + i].px, static_cast<float>(i), 1e-5f);
        EXPECT_NEAR(out[kRegionSpan + i].py, static_cast<float>(i) * 2.0f + 100.0f, 1e-5f)
            << "submesh 1 read submesh 0's parameter block";
    }
}

TEST_P(ModifierGpuTest, SkinningThenAnAppModifierDisplacesTheSkinnedVertex) {
    const auto vertices = MakeVertices();
    const SkinEntry entry = MakeSkinnedEntry(0);

    // 255 on joint 0, whose palette translates by +5 in X: the skinning stage
    // moves every vertex, and the app stage then displaces the result in Y.
    std::vector<GpuBoneInfluence> influences(vertices.size());
    for (GpuBoneInfluence& b : influences) {
        b.joints01 = 0;
        b.weights = 255u;
    }
    const std::array<glm::mat4, 1> palettes{
        glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f))};

    std::array<std::byte, 256> params{};
    PutParams(params, 0, Translate(0.0f, 7.0f, 0.0f));

    const std::array<ModifierDispatch, 2> dispatches{
        // The builtin skinning stage: param_size 0, prepended by gather.
        MakeDispatch(0, 0, modifier_mgr_.SkinningId(),
                     static_cast<std::uint32_t>(vertices.size()), 0, 0, 0),
        MakeDispatch(0, 1, app_id_, static_cast<std::uint32_t>(vertices.size()), 0, 0),
    };

    Configure(vertices, std::span<const SkinEntry>(&entry, 1), dispatches, params, palettes, influences);
    const auto out = Run();

    for (std::uint32_t i = 0; i < vertices.size(); ++i) {
        // Stage 0 (skinning) wrote A: p + (5,0,0). Stage 1 (app) read A and wrote
        // B: p + (5,7,0).
        EXPECT_NEAR(out[i].px, static_cast<float>(i) + 5.0f, 1e-5f) << "element " << i;
        EXPECT_NEAR(out[kFrameSpan + i].px, static_cast<float>(i) + 5.0f, 1e-5f)
            << "element " << i;
        EXPECT_NEAR(out[kFrameSpan + i].py, static_cast<float>(i) * 2.0f + 7.0f, 1e-5f)
            << "the app modifier did not run on the skinned result";
        EXPECT_NEAR(out[kFrameSpan + i].pz, static_cast<float>(i) * 3.0f, 1e-5f);
    }
}

TEST_P(ModifierGpuTest, ARoundedUpDispatchNeverWritesItsNeighboursRegion) {
    const auto vertices = MakeVertices();
    const std::array<SkinEntry, 2> entries{MakeUnskinnedEntry(0),
                                           MakeUnskinnedEntry(kRegionSpan)};

    // Pre-fill both regions with a sentinel by running two full-span constant
    // dispatches, so "untouched" is observable (device-local memory starts
    // undefined).
    std::array<std::byte, 256> fill_params{};
    PutParams(fill_params, 0, ConstantAt(-1.0f, -2.0f, -3.0f));
    const std::array<ModifierDispatch, 2> fills{
        MakeDispatch(0, 0, app_id_, static_cast<std::uint32_t>(vertices.size()), 0, 0),
        MakeDispatch(1, 0, app_id_, static_cast<std::uint32_t>(vertices.size()), 0, 0),
    };
    Configure(vertices, entries, fills, fill_params);
    const auto filled = Run();
    for (std::uint32_t i = 0; i < kFrameSpan; ++i) {
        ASSERT_FLOAT_EQ(filled[i].px, -1.0f) << "sentinel fill failed at element " << i;
    }

    // One vertex dispatched with workgroup_size 64: 63 threads run past
    // vertex_count. The early-out must stop them from writing the rest of region
    // A, and nothing may touch submesh 1's region at all.
    std::array<std::byte, 256> params{};
    PutParams(params, 0, Translate(10.0f, 20.0f, 30.0f));
    const ModifierDispatch one_vertex = MakeDispatch(0, 0, app_id_, /*vertex_count=*/1);
    Configure(vertices, entries, std::span<const ModifierDispatch>(&one_vertex, 1), params);
    const auto out = Run();

    EXPECT_NEAR(out[0].px, 10.0f, 1e-5f) << "the dispatched vertex was not written";
    EXPECT_NEAR(out[0].py, 20.0f, 1e-5f);
    EXPECT_NEAR(out[0].pz, 30.0f, 1e-5f);
    for (std::uint32_t i = 1; i < kRegionSpan; ++i) {
        EXPECT_FLOAT_EQ(out[i].px, -1.0f)
            << "trailing thread " << i << " wrote past vertex_count and corrupted its "
               "own submesh's region";
    }
    for (std::uint32_t i = kRegionSpan; i < kFrameSpan; ++i) {
        EXPECT_FLOAT_EQ(out[i].px, -1.0f)
            << "the rounded-up dispatch corrupted the neighbouring submesh's region";
    }
}

TEST_P(ModifierGpuTest, ARejectedRegionIsNeverWrittenByAnAppModifier) {
    const auto vertices = MakeVertices();
    // Submesh 0 got a region; submesh 1 did not. expand clears kDeformActive and
    // leaves deform_base at the 0 gather wrote, so submesh 1's stage must not run
    // its write: dstBase would be 0, which is submesh 0's region.
    const std::array<SkinEntry, 2> entries{MakeUnskinnedEntry(0),
                                           MakeRejectedEntry(0)};

    std::array<std::byte, 256> params{};
    PutParams(params, 0, Translate(10.0f, 20.0f, 30.0f));
    PutParams(params, 16, ConstantAt(-99.0f, -99.0f, -99.0f));

    const std::array<ModifierDispatch, 2> dispatches{
        MakeDispatch(0, 0, app_id_, static_cast<std::uint32_t>(vertices.size()), 0, 0),
        MakeDispatch(1, 0, app_id_, static_cast<std::uint32_t>(vertices.size()), 0, 16),
    };

    Configure(vertices, entries, dispatches, params);
    const auto out = Run();

    for (std::uint32_t i = 0; i < vertices.size(); ++i) {
        EXPECT_NEAR(out[i].px, static_cast<float>(i) + 10.0f, 1e-5f) << "element " << i;
        EXPECT_NE(out[i].px, -99.0f)
            << "the submesh whose region could not be allocated wrote into another "
               "submesh's region at element " << i;
    }
}

TEST_P(ModifierGpuTest, ARetiredModifierIsNoLongerDispatched) {
    const auto vertices = MakeVertices();
    const SkinEntry entry = MakeUnskinnedEntry(0);

    std::array<std::byte, 256> fill_params{};
    PutParams(fill_params, 0, ConstantAt(-1.0f, -2.0f, -3.0f));
    const ModifierDispatch fill = MakeDispatch(0, 0, app_id_,
                                               static_cast<std::uint32_t>(vertices.size()));
    Configure(vertices, std::span<const SkinEntry>(&entry, 1), std::span<const ModifierDispatch>(&fill, 1), fill_params);
    const auto filled = Run();
    ASSERT_FLOAT_EQ(filled[0].px, -1.0f);

    // Unregister while the frame's dispatch list still names the handle: the
    // retire path must stop DispatchDeform from using a pipeline whose slot is
    // being freed (gather would already have dropped the stage).
    std::array<std::byte, 256> params{};
    PutParams(params, 0, Translate(10.0f, 20.0f, 30.0f));
    const ModifierDispatch stale = MakeDispatch(0, 0, app_id_,
                                                static_cast<std::uint32_t>(vertices.size()));
    Configure(vertices, std::span<const SkinEntry>(&entry, 1), std::span<const ModifierDispatch>(&stale, 1), params);

    modifier_mgr_.Unregister(app_id_);
    renderer_->SyncModifierPipelines(1);
    EXPECT_FALSE(renderer_->IsModifierReady(app_id_))
        << "a retired modifier is still reported ready";

    const auto out = Run();
    for (std::uint32_t i = 0; i < kRegionSpan; ++i) {
        EXPECT_FLOAT_EQ(out[i].px, -1.0f)
            << "a retired modifier's pipeline was still dispatched at element " << i;
    }
}

INSTANTIATE_TEST_SUITE_P(BothDrawModes, ModifierGpuTest,
                         ::testing::Values(DrawMode::CID, DrawMode::MID),
                         [](const ::testing::TestParamInfo<DrawMode>& info) {
                             return info.param == DrawMode::MID ? "MID" : "CID";
                         });

}  // namespace
