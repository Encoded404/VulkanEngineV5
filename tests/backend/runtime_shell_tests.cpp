#include <gtest/gtest.h>

import VulkanBackend.Runtime.FrameLoop;

namespace {

using namespace VulkanEngine::Runtime;

TEST(RuntimeShellTest, ResizeAndOutOfDateStatusesAreReported) {
    RuntimeShell runtime{};
    ASSERT_TRUE(runtime.Initialize(RuntimeConfig{.frames_in_flight = 3}));

    runtime.NotifyWindowResized();
    const auto resized = runtime.BeginFrame();
    EXPECT_EQ(resized.status, RuntimeStatus::ResizePending);

    runtime.EndFrame();
    const auto after_resize = runtime.BeginFrame();
    EXPECT_EQ(after_resize.status, RuntimeStatus::Ok);

    runtime.NotifySwapchainOutOfDate();
    const auto out_of_date = runtime.BeginFrame();
    EXPECT_EQ(out_of_date.status, RuntimeStatus::SwapchainOutOfDate);
}

TEST(RuntimeShellTest, MinimizedStateBlocksNormalFrameStatus) {
    RuntimeShell runtime{};
    ASSERT_TRUE(runtime.Initialize(RuntimeConfig{}));

    runtime.NotifyWindowMinimized(true);
    const auto minimized = runtime.BeginFrame();
    EXPECT_EQ(minimized.status, RuntimeStatus::Minimized);

    runtime.NotifyWindowMinimized(false);
    const auto restored = runtime.BeginFrame();
    EXPECT_EQ(restored.status, RuntimeStatus::Ok);
}

TEST(RuntimeShellTest, ShutdownRequestIsObservable) {
    RuntimeShell runtime{};
    ASSERT_TRUE(runtime.Initialize(RuntimeConfig{}));

    EXPECT_FALSE(runtime.ShouldShutdown());
    runtime.RequestShutdown();
    EXPECT_TRUE(runtime.ShouldShutdown());
}

}  // namespace
