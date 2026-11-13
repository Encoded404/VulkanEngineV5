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
import VulkanEngine.GpuResources;
import VulkanEngine.ModifierManager;

namespace {

using VulkanEngine::GpuResources::DeviceBufferHeap;
using VulkanEngine::GpuResources::GpuBuffer;
using VulkanEngine::GpuResources::HeapConfig;
using VulkanEngine::Modifiers::ModifierManager;
using VulkanEngine::SceneRenderer::DrawMode;
using VulkanEngine::SceneRenderer::SceneCapacity;
using VulkanEngine::SceneRenderer::SceneRenderer;
using VulkanEngine::ShaderSystem::PipelineFactory;
using VulkanEngine::ShaderSystem::ShaderManager;

// The per-object deform bounds bits, end to end through the real cull shaders:
// a submesh with kCullNoCullTest must survive a frustum/Hi-Z cull (its
// rest-pose bounds are stale) and a submesh with kCullNoOccluder must never be
// selected as an occluder. The control entry in each test proves the test
// geometry really would have been culled/selected without the bit.
class SkinnedBoundsGpuTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!TestSupport::IsGpuDeviceAvailable()) {
            GTEST_SKIP() << "no Vulkan device available";
        }
        ASSERT_TRUE(backend_.Initialize());
        ASSERT_TRUE(vertex_heap_.Initialize(
            backend_, HeapConfig{.block_size = 4ULL << 20}, "bounds-test-heap"));
        shaders_ = std::make_unique<ShaderManager>(backend_.GetDevice(),
                                                   backend_.GetCapabilities(),
                                                   std::filesystem::temp_directory_path().string());
        ids_.RegisterAll(*shaders_, VKENGINE_ENGINE_SHADER_DIR);
        pipeline_factory_ = std::make_unique<PipelineFactory>(
            backend_.GetDevice(), backend_.GetCapabilities(),
            shaders_->GetPipelineCacheRAII());

        const SceneCapacity capacity{
            .index_count = 256,
            .vertex_span = 256,
            .submesh_count = 8,
            .deform_vertex_span = 8,
            .palette_matrix_count = 8,
            .modifier_param_bytes = 256,
        };
        renderer_ = std::make_unique<SceneRenderer>();
        ASSERT_TRUE(renderer_->Initialize(backend_, vertex_heap_, capacity, *shaders_,
                                          *pipeline_factory_, ids_, /*frames_in_flight=*/1,
                                          DrawMode::CID, modifier_mgr_));
        // A headless bootstrap has no swapchain extent; the pre-cull dispatch
        // binds the Hi-Z descriptors, so give the renderer the test extent before
        // PrepareCompute writes them.
        renderer_->EnsureRenderExtent(kWidth, kHeight);

        // One submesh in the frame: PrepareCompute grows the block arrays and
        // writes the cull-pass descriptors for it.
        renderer_->SetCurrentEntityCount(1);
        renderer_->PrepareCompute({}, registry_, view_, projection_, kWidth, kHeight, 0);
    }

    void TearDown() override {
        if (!TestSupport::IsGpuDeviceAvailable()) {
            return;
        }
        staging_keepalive_.clear();
        renderer_->Shutdown();
        renderer_.reset();
        pipeline_factory_.reset();
        shaders_.reset();
        vertex_heap_.Shutdown();
        backend_.Shutdown();
    }

    void SeedSubmesh(const std::uint32_t index, const std::uint32_t cull_flags,
                     const bool large_on_screen) {
        using VulkanEngine::SceneRenderer::BoundingSphereEntry;
        using VulkanEngine::SceneRenderer::CullEntry;
        using VulkanEngine::SceneRenderer::ObbEntry;
        using VulkanEngine::SceneRenderer::VertexEntry;

        auto& vertex_blocks = renderer_->GetFrameVertexEntries(0);
        auto& cull_blocks = renderer_->GetFrameCullEntries(0);
        auto& sphere_blocks = *renderer_->GetFrameBlockArrays(0).bounding_spheres;
        auto& obb_blocks = *renderer_->GetFrameBlockArrays(0).obb_entries;

        // Bounding spheres and OBBs are host-visible: write them directly.
        auto* sphere = static_cast<BoundingSphereEntry*>(sphere_blocks.Get(index));
        ASSERT_NE(sphere, nullptr);
        auto* obb = static_cast<ObbEntry*>(obb_blocks.Get(index));
        ASSERT_NE(obb, nullptr);

        // The vertex and cull entry blocks are GPU-owned (device-local), so the
        // images are staged and copied into the block on the recording path.
        if (vertex_image_.empty()) {
            vertex_image_.assign(vertex_blocks.BlockSize(), std::byte{0});
            cull_image_.assign(cull_blocks.BlockSize(), std::byte{0});
        }

        const glm::mat4 mvp = projection_ * view_;
        VertexEntry vertex{};
        std::memcpy(vertex.mvp.data(), &mvp, sizeof(glm::mat4));
        vertex.max_scale = 1.0f;
        vertex.vertex_buffer_slot = 0;
        std::memcpy(vertex_image_.data() + static_cast<std::size_t>(index) * vertex_blocks.EntrySize(),
                    &vertex, sizeof(vertex));

        CullEntry cull{};
        cull.index_base = 0;
        cull.index_count = 3;
        cull.technique_id = 0;
        cull.flags = cull_flags;
        std::memcpy(cull_image_.data() + static_cast<std::size_t>(index) * cull_blocks.EntrySize(),
                    &cull, sizeof(cull));

        if (large_on_screen) {
            *obb = ObbEntry{};
            obb->cz = -5.0f;
            obb->ux = 1.0f;
            obb->hu = 3.0f;
            obb->vy = 1.0f;
            obb->hv = 3.0f;
            obb->wz = 1.0f;
            obb->hw = 3.0f;
            // Passes the frustum test; the OBB area decides selection.
            *sphere = BoundingSphereEntry{0.0f, 0.0f, -5.0f, 1.0f};
        } else {
            // In front of the camera (clip w > 0) but far off to the side, so the
            // frustum test is what culls it.
            *sphere = BoundingSphereEntry{50.0f, 0.0f, -10.0f, 0.5f};
            *obb = ObbEntry{};
        }
    }

    // Copies a staged image into the first block of a device-local array.
    void CopyImageInto(vk::CommandBuffer cmd,
                       VulkanEngine::GpuResources::BlockArray& blocks,
                       const std::vector<std::byte>& image) {
        if (image.empty() || blocks.BlockCount() == 0) return;
        auto staging = GpuBuffer::Create(backend_, image.size(),
                                         vk::BufferUsageFlagBits::eTransferSrc,
                                         vk::MemoryPropertyFlagBits::eHostVisible |
                                             vk::MemoryPropertyFlagBits::eHostCoherent);
        staging.Upload(image.data(), image.size());
        cmd.copyBuffer(static_cast<vk::Buffer>(*staging.GetBuffer()),
                       blocks.GetBlockArray(0), vk::BufferCopy(0, 0, image.size()));
        const vk::BufferMemoryBarrier barrier(
            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, blocks.GetBlockArray(0), 0,
            vk::WholeSize);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eComputeShader, {}, {}, barrier, {});
        staging_keepalive_.push_back(std::move(staging));
    }

    // Records one cull pass and copies submesh 0's cull entry back.
    [[nodiscard]] VulkanEngine::SceneRenderer::CullEntry ReadCullAfter(
        const std::function<void(vk::CommandBuffer)>& dispatch) {
        const auto& blocks = renderer_->GetFrameCullEntries(0);
        const vk::Buffer src = blocks.GetBlockArray(0);
        const vk::DeviceSize bytes = sizeof(VulkanEngine::SceneRenderer::CullEntry);

        auto readback = GpuBuffer::Create(backend_, blocks.BlockSize(),
                                          vk::BufferUsageFlagBits::eTransferDst,
                                          vk::MemoryPropertyFlagBits::eHostVisible |
                                              vk::MemoryPropertyFlagBits::eHostCoherent);
        vk::raii::CommandBuffer& cmd = backend_.GetCommandBuffer(0);
        cmd.reset({});
        cmd.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        CopyImageInto(cmd, renderer_->GetFrameVertexEntries(0), vertex_image_);
        CopyImageInto(cmd, renderer_->GetFrameCullEntries(0), cull_image_);
        dispatch(cmd);
        const vk::BufferMemoryBarrier to_copy(
            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead,
            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, src, 0, vk::WholeSize);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, to_copy, {});
        cmd.copyBuffer(src, static_cast<vk::Buffer>(*readback.GetBuffer()),
                       vk::BufferCopy(0, 0, bytes));
        cmd.end();
        backend_.GetGraphicsQueue().submit(vk::SubmitInfo(0, nullptr, nullptr, 1, &*cmd), {});
        backend_.GetGraphicsQueue().waitIdle();

        VulkanEngine::SceneRenderer::CullEntry out{};
        const void* mapped = readback.Map(0, bytes);
        std::memcpy(&out, mapped, sizeof(out));
        readback.Unmap();
        return out;
    }

    static constexpr std::uint32_t kWidth = 1280;
    static constexpr std::uint32_t kHeight = 720;

    TestSupport::HeadlessVulkanBackend backend_{};
    DeviceBufferHeap vertex_heap_{};
    ModifierManager modifier_mgr_{};
    VulkanEngine::ComponentRegistry registry_{};
    std::unique_ptr<ShaderManager> shaders_;
    std::unique_ptr<PipelineFactory> pipeline_factory_;
    VulkanEngine::EngineShaderIds ids_{};
    std::unique_ptr<SceneRenderer> renderer_;
    std::vector<std::byte> vertex_image_;
    std::vector<std::byte> cull_image_;
    std::vector<VulkanEngine::GpuResources::GpuBuffer> staging_keepalive_;

    glm::mat4 view_{glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f),
                                glm::vec3(0.0f, 1.0f, 0.0f))};
    glm::mat4 projection_{
        glm::perspective(glm::radians(60.0f), 1280.0f / 720.0f, 0.1f, 100.0f)};
};

}  // namespace

TEST_F(SkinnedBoundsGpuTest, NoCullTestBitSurvivesTheFrustumCull) {
    // Technique flags: depth pass on (so the survivor is compacted), occlusion
    // off (this test targets the frustum test, not Hi-Z).
    const std::array<std::uint32_t, 2> flags{
        VulkanEngine::SceneRenderer::TECHNIQUE_FLAG_DEPTH_PASS, 0u};
    renderer_->SetTechniqueFlags(flags);

    // Control: the same off-screen submesh without the bit is culled.
    SeedSubmesh(0, 0u, false);
    const auto culled = ReadCullAfter([this](vk::CommandBuffer cmd) { renderer_->DispatchPreCull(cmd, 0); });
    EXPECT_EQ(culled.index_count, 0u) << "the control submesh should have been frustum-culled";

    // With kCullNoCullTest it survives.
    SeedSubmesh(0, VulkanEngine::SceneRenderer::kCullNoCullTest, false);
    const auto kept = ReadCullAfter([this](vk::CommandBuffer cmd) { renderer_->DispatchPreCull(cmd, 0); });
    EXPECT_EQ(kept.index_count, 3u)
        << "a deformed submesh was culled against its stale rest-pose bounds";
}

TEST_F(SkinnedBoundsGpuTest, NoOccluderBitKeepsTheSubmeshOutOfTheOccluderSet) {
    // A technique that is otherwise a perfect occluder: depth pass and
    // conservative bounds.
    const std::array<std::uint32_t, 2> flags{
        VulkanEngine::SceneRenderer::TECHNIQUE_FLAG_DEPTH_PASS |
            VulkanEngine::SceneRenderer::TECHNIQUE_FLAG_OCCLUDER_SAFE,
        0u};
    renderer_->SetTechniqueFlags(flags);

    // Control: an identical on-screen submesh without the bit is selected.
    SeedSubmesh(0, 0u, true);
    const auto selected = ReadCullAfter([this](vk::CommandBuffer cmd) {
        renderer_->DispatchOccluderSelect(cmd, kWidth, kHeight, 0);
    });
    EXPECT_NE(selected.flags & VulkanEngine::SceneRenderer::kOccluderBit, 0u)
        << "the control submesh should have been selected as an occluder";

    // With kCullNoOccluder it is skipped even though it is large and on screen.
    SeedSubmesh(0, VulkanEngine::SceneRenderer::kCullNoOccluder, true);
    const auto skipped = ReadCullAfter([this](vk::CommandBuffer cmd) {
        renderer_->DispatchOccluderSelect(cmd, kWidth, kHeight, 0);
    });
    EXPECT_EQ(skipped.flags & VulkanEngine::SceneRenderer::kOccluderBit, 0u)
        << "a deformed submesh was selected as an occluder on its rest-pose bounds";
}
