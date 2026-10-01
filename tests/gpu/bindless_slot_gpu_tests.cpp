#include <gtest/gtest.h>

import std;

import vulkan_hpp;
import test_gpu;
import TestSupport.HeadlessVulkanBackend;
import VulkanBackend.Vulkan.VulkanBootstrap;
import VulkanEngine.ResourceSystem;
import VulkanEngine.BindlessManager;
import VulkanEngine.TextureTypes;
import VulkanEngine.GpuResources.GpuImageHeap;
import VulkanEngine.GpuTexture;

namespace {

using namespace VulkanEngine::BindlessManager;
using VulkanEngine::GpuResources::GpuImageHeap;
using VulkanEngine::GpuResources::GpuTexture;

// Headless bindless checks for the C07 slot rules. The bindless manager needs a
// real device (descriptor set + pool + update-after-bind), so these stay on the
// gpu label. The slot arithmetic they assert is the same logic the engine
// bootstraps with.
class BindlessFallbackGpuTest : public ::testing::Test {
protected:
    TestSupport::HeadlessVulkanBackend backend{};
    GpuImageHeap heap{};
    std::unique_ptr<BindlessManager> bindless{};

    void SetUp() override {
        if (!TestSupport::IsGpuDeviceAvailable()) {
            GTEST_SKIP() << "no Vulkan device available";
        }
        ASSERT_TRUE(backend.Initialize());
        ASSERT_TRUE(heap.Initialize(backend, VulkanEngine::GpuResources::ImageHeapConfig{}, "bindless-test"));
        bindless = std::make_unique<BindlessManager>();
        ASSERT_TRUE(bindless->Initialize(backend));
    }

    void TearDown() override {
        if (!TestSupport::IsGpuDeviceAvailable()) {
            return;
        }
        if (bindless) {
            bindless->Shutdown();
            bindless.reset();
        }
        heap.Shutdown();
        backend.Shutdown();
    }

    [[nodiscard]] GpuTexture MakeSolidTexture(std::uint8_t value) {
        VulkanEngine::Textures::TextureData data{};
        data.width = 2;
        data.height = 2;
        data.source_format = vk::Format::eR8G8B8A8Unorm;
        data.source_channels = 4;
        data.source_alpha = 1;
        data.subresources.push_back(VulkanEngine::Textures::TextureSubresource{
            .mip = 0, .array_layer = 0, .offset = 0, .size = 16,
            .width = 2, .height = 2, .depth = 1,
        });
        data.blob.assign(16, static_cast<std::byte>(value));
        return GpuTexture::CreateFromTextureData(backend, heap, data, vk::Format::eR8G8B8A8Unorm);
    }
};

// Slot 0 is the permanent fallback; allocation starts at slot 1 and the
// fallback texture stays resolvable at GetTexture(0).
TEST_F(BindlessFallbackGpuTest, FallbackOwnsSlotZeroAndAllocationStartsAtOne) {
    bindless->SetFallback(MakeSolidTexture(0xAA), VulkanEngine::ResourceId{"checkerboard_default"});

    EXPECT_NE(bindless->GetTexture(kFallbackSlot), nullptr);
    EXPECT_NE(bindless->GetTexture(kFallbackSlot)->GetImageView(), vk::ImageView{nullptr});
    EXPECT_NE(bindless->GetTexture(kFallbackSlot)->GetSampler(), vk::Sampler{nullptr});

    const std::uint32_t first = bindless->AllocateTextureSlot(MakeSolidTexture(0x11), VulkanEngine::ResourceId{"first"});
    EXPECT_EQ(first, kFallbackSlot + 1U);
    const std::uint32_t second = bindless->AllocateTextureSlot(MakeSolidTexture(0x22), VulkanEngine::ResourceId{"second"});
    EXPECT_EQ(second, kFallbackSlot + 2U);
    EXPECT_EQ(bindless->GetTextureId(first)->value, "first");
    EXPECT_EQ(bindless->GetTextureId(second)->value, "second");
    // The fallback is untouched by the allocations.
    EXPECT_EQ(bindless->GetTextureId(kFallbackSlot)->value, "checkerboard_default");
}

// Second SetFallback is ignored: slot 0 is written exactly once (the fallback
// is never replaced or re-uploaded).
TEST_F(BindlessFallbackGpuTest, SetFallbackIsOneShot) {
    bindless->SetFallback(MakeSolidTexture(0xAA), VulkanEngine::ResourceId{"checkerboard_default"});
    bindless->SetFallback(MakeSolidTexture(0xBB), VulkanEngine::ResourceId{"imposter"});
    EXPECT_EQ(bindless->GetTextureId(kFallbackSlot)->value, "checkerboard_default");
    // The imposter texture was not allocated a slot either.
    const std::uint32_t next = bindless->AllocateTextureSlot(MakeSolidTexture(0x11), VulkanEngine::ResourceId{"next"});
    EXPECT_EQ(next, 1U);
}

// Shutdown (the fixture's TearDown) must not leak textures_; the heap Shutdown
// after it verifies the teardown ordering (bindless before heap).
TEST_F(BindlessFallbackGpuTest, ShutdownClearsSlotsAndFallback) {
    bindless->SetFallback(MakeSolidTexture(0xAA), VulkanEngine::ResourceId{"checkerboard_default"});
    static_cast<void>(bindless->AllocateTextureSlot(MakeSolidTexture(0x11), VulkanEngine::ResourceId{"first"}));
    bindless->Shutdown();
    EXPECT_EQ(bindless->GetTexture(kFallbackSlot), nullptr);
    EXPECT_EQ(bindless->GetTexture(1U), nullptr);
    bindless.reset();
    heap.Shutdown(); // must not crash with textures already freed
    SUCCEED();
}

}  // namespace