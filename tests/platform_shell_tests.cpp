#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include <SDL3/SDL_video.h>

import VulkanBackend.Event;
import VulkanBackend.Platform.SdlPlatform;

namespace {

using namespace VulkanEngine::Platform;

class FakePlatformBackend final : public IPlatformBackend {
public:
    bool initialize_result = true; // NOLINT(misc-non-private-member-variables-in-classes)
    bool create_window_result = true; // NOLINT(misc-non-private-member-variables-in-classes)
    bool shutdown_called = false; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Backend::Event::EventList queued_events{}; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool Initialize() override {
        return initialize_result;
    }

    void Shutdown() override {
        shutdown_called = true;
    }

    [[nodiscard]] bool CreateMainWindow(const PlatformConfig&) override {
        return create_window_result;
    }

    [[nodiscard]] VulkanEngine::Backend::Event::EventList PumpEvents() override {
        return std::exchange(queued_events, {});
    }

    [[nodiscard]] SDL_Window* GetNativeWindowHandle() const override {
        return nullptr;
    }
};

TEST(PlatformShellTest, InitializesAndShutsDownThroughBackend) {
    auto backend = std::make_shared<FakePlatformBackend>();
    SdlPlatformShell shell(backend);

    ASSERT_TRUE(shell.Initialize(PlatformConfig{.window_title = "test", .window_width = 800, .window_height = 600}));
    EXPECT_TRUE(shell.IsInitialized());
    EXPECT_EQ(shell.GetState().status, PlatformStatus::Ok);

    shell.Shutdown();
    EXPECT_TRUE(backend->shutdown_called);
    EXPECT_FALSE(shell.IsInitialized());
}

TEST(PlatformShellTest, PollEventsUpdatesResizeAndQuitState) {
    auto backend = std::make_shared<FakePlatformBackend>();
    SdlPlatformShell shell(backend);

    ASSERT_TRUE(shell.Initialize(PlatformConfig{}));

    backend->queued_events.push_back(std::make_unique<VulkanEngine::Backend::Event::WindowResizedEvent>(1920u, 1080u));
    backend->queued_events.push_back(std::make_unique<VulkanEngine::Backend::Event::QuitEvent>());

    const auto events = shell.PollEvents();

    const auto& state = shell.GetState();
    EXPECT_TRUE(state.resized);
    EXPECT_EQ(state.drawable_width, 1920u);
    EXPECT_EQ(state.drawable_height, 1080u);
    EXPECT_TRUE(shell.ShouldQuit());
    EXPECT_EQ(state.status, PlatformStatus::QuitRequested);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_NE(dynamic_cast<VulkanEngine::Backend::Event::WindowResizedEvent*>(events[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<VulkanEngine::Backend::Event::QuitEvent*>(events[1].get()), nullptr);
}

TEST(PlatformShellTest, ReportsBackendInitializationFailure) {
    auto backend = std::make_shared<FakePlatformBackend>();
    backend->initialize_result = false;

    SdlPlatformShell shell(backend);
    EXPECT_FALSE(shell.Initialize(PlatformConfig{}));
    EXPECT_EQ(shell.GetState().status, PlatformStatus::BackendInitFailed);
}

}  // namespace

