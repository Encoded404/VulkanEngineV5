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
import VulkanEngine.ModifierId;

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
using VulkanEngine::StandardMeshPipeline::Vertex;

// The "unskinned scenes are untouched" guard: with no skinning modifier in the
// registry and no dispatch list, the deform stage has nothing to record and the
// deformed-vertex buffer is never written. Pixel identity of the unskinned path
// is pinned separately by the golden-hash render tests (unlit_render_gpu_tests,
// uv_variant_gpu_tests, mask_variant_gpu_tests, normal_mapping_gpu_tests); this
// test pins the other half of the criterion -- zero dispatches, zero writes.
class UnskinnedRegressionGpuTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!TestSupport::IsGpuDeviceAvailable()) {
            GTEST_SKIP() << "no Vulkan device available";
        }
        ASSERT_TRUE(backend_.Initialize());
        ASSERT_TRUE(vertex_heap_.Initialize(
            backend_, HeapConfig{.block_size = 4ULL << 20}, "unskinned-test-heap"));

        shaders_ = std::make_unique<ShaderManager>(backend_.GetDevice(),
                                                   backend_.GetCapabilities(),
                                                   std::filesystem::temp_directory_path().string());
        ids_.RegisterAll(*shaders_, VKENGINE_ENGINE_SHADER_DIR);
        pipeline_factory_ = std::make_unique<PipelineFactory>(
            backend_.GetDevice(), backend_.GetCapabilities(),
            shaders_->GetPipelineCacheRAII());

        const SceneCapacity capacity{
            .index_count = 1024,
            .vertex_span = 1024,
            .submesh_count = 64,
            .deform_vertex_span = 8,
            .palette_matrix_count = 8,
            .modifier_param_bytes = 256,
        };
        renderer_ = std::make_unique<SceneRenderer>();
        ASSERT_TRUE(renderer_->Initialize(backend_, vertex_heap_, capacity, *shaders_,
                                          *pipeline_factory_, ids_, /*frames_in_flight=*/1,
                                          DrawMode::CID, modifier_mgr_));
    }

    void TearDown() override {
        if (!TestSupport::IsGpuDeviceAvailable()) {
            return;
        }
        renderer_->Shutdown();
        renderer_.reset();
        pipeline_factory_.reset();
        shaders_.reset();
        vertex_heap_.Shutdown();
        backend_.Shutdown();
    }

    // Fills the deformed-vertex buffer with a sentinel byte pattern, runs the
    // deform pass, and reads the buffer back. A dispatch that ran would
    // overwrite at least one Vertex-sized chunk.
    [[nodiscard]] std::vector<std::byte> RunDeformAndReadBack() {
        const vk::Buffer deform = renderer_->GetFrameDeformVertices(0);
        EXPECT_TRUE(deform) << "no deform vertex buffer";
        constexpr vk::DeviceSize kBytes = 2 * 8 * sizeof(Vertex);

        std::vector<std::byte> sentinel(static_cast<std::size_t>(kBytes), std::byte{0xAB});
        auto staging = GpuBuffer::Create(backend_, kBytes,
                                         vk::BufferUsageFlagBits::eTransferSrc,
                                         vk::MemoryPropertyFlagBits::eHostVisible |
                                             vk::MemoryPropertyFlagBits::eHostCoherent);
        auto readback = GpuBuffer::Create(backend_, kBytes,
                                          vk::BufferUsageFlagBits::eTransferDst,
                                          vk::MemoryPropertyFlagBits::eHostVisible |
                                              vk::MemoryPropertyFlagBits::eHostCoherent);
        staging.Upload(sentinel.data(), kBytes);

        vk::raii::CommandBuffer& cmd = backend_.GetCommandBuffer(0);
        cmd.reset({});
        cmd.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        cmd.copyBuffer(static_cast<vk::Buffer>(*staging.GetBuffer()), deform,
                       vk::BufferCopy(0, 0, kBytes));
        const vk::BufferMemoryBarrier fill_barrier(
            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, deform, 0, vk::WholeSize);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                            vk::PipelineStageFlagBits::eComputeShader, {}, {}, fill_barrier,
                            {});
        renderer_->DispatchDeform(cmd, 0);
        const vk::BufferMemoryBarrier to_copy(
            vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eTransferRead,
            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, deform, 0, vk::WholeSize);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                            vk::PipelineStageFlagBits::eTransfer, {}, {}, to_copy, {});
        cmd.copyBuffer(deform, static_cast<vk::Buffer>(*readback.GetBuffer()),
                       vk::BufferCopy(0, 0, kBytes));
        cmd.end();
        backend_.GetGraphicsQueue().submit(vk::SubmitInfo(0, nullptr, nullptr, 1, &*cmd), {});
        backend_.GetGraphicsQueue().waitIdle();

        std::vector<std::byte> out(static_cast<std::size_t>(kBytes));
        const void* mapped = readback.Map(0, kBytes);
        std::memcpy(out.data(), mapped, out.size());
        readback.Unmap();
        return out;
    }

    TestSupport::HeadlessVulkanBackend backend_{};
    DeviceBufferHeap vertex_heap_{};
    ModifierManager modifier_mgr_{};
    std::unique_ptr<ShaderManager> shaders_;
    std::unique_ptr<PipelineFactory> pipeline_factory_;
    VulkanEngine::EngineShaderIds ids_{};
    std::unique_ptr<SceneRenderer> renderer_;
};

TEST_F(UnskinnedRegressionGpuTest, NoSkinningModifierMeansNoDispatchAndNoWrite) {
    // No builtin skinning entry was registered: gather has nothing to prepend
    // and the renderer reports no ready skinning pipeline.
    EXPECT_FALSE(renderer_->SkinningModifier().IsValid());
    EXPECT_FALSE(renderer_->IsModifierReady(renderer_->SkinningModifier()));

    // Gather publishes an empty dispatch list for a frame with no deform.
    renderer_->SetModifierDispatches(0, {});
    renderer_->SetHasActiveDeform(false);
    EXPECT_TRUE(renderer_->GetModifierDispatches(0).empty());
    EXPECT_FALSE(renderer_->HasActiveDeform());

    const auto before = RunDeformAndReadBack();
    // Every byte is still the sentinel: the deform pass recorded nothing.
    for (const std::byte b : before) {
        ASSERT_EQ(b, std::byte{0xAB}) << "DispatchDeform wrote into an unskinned frame";
    }
}

TEST_F(UnskinnedRegressionGpuTest, DeformBufferIsAllocatedButNeverDispatchedInto) {
    // The buffer exists so the set-2 descriptor binding is always valid, even
    // for scenes that never deform anything.
    EXPECT_TRUE(renderer_->GetFrameDeformVertices(0));
    renderer_->SetModifierDispatches(0, {});
    EXPECT_TRUE(renderer_->GetModifierDispatches(0).empty());
}

}  // namespace
