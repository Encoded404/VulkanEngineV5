#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include <SDL3/SDL_video.h>

import VulkanBackend.Platform.SdlPlatform;

namespace {

using namespace VulkanEngine::Platform;

class FakePlatformBackend final : public IPlatformBackend {
public:
    bool initialize_result = true; // NOLINT(misc-non-private-member-variables-in-classes)
    bool create_window_result = true; // NOLINT(misc-non-private-member-variables-in-classes)
    bool shutdown_called = false; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<PlatformEvent> queued_events{}; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool Initialize() override {
        return initialize_result;
    }

    void Shutdown() override {
        shutdown_called = true;
    }

    [[nodiscard]] bool CreateMainWindow(const PlatformConfig&) override {
        return create_window_result;
    }

    [[nodiscard]] std::vector<PlatformEvent> PumpEvents() override {
        auto copy = queued_events;
        queued_events.clear();
        return copy;
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

    backend->queued_events.push_back(PlatformEvent{.type = PlatformEventType::WindowResized, .width = 1920, .height = 1080});
    backend->queued_events.push_back(PlatformEvent{.type = PlatformEventType::Quit});

    shell.PollEvents();

    const auto& state = shell.GetState();
    EXPECT_TRUE(state.resized);
    EXPECT_EQ(state.drawable_width, 1920u);
    EXPECT_EQ(state.drawable_height, 1080u);
    EXPECT_TRUE(shell.ShouldQuit());
    EXPECT_EQ(state.status, PlatformStatus::QuitRequested);
}

TEST(PlatformShellTest, ReportsBackendInitializationFailure) {
    auto backend = std::make_shared<FakePlatformBackend>();
    backend->initialize_result = false;

    SdlPlatformShell shell(backend);
    EXPECT_FALSE(shell.Initialize(PlatformConfig{}));
    EXPECT_EQ(shell.GetState().status, PlatformStatus::BackendInitFailed);
}

}  // namespace

