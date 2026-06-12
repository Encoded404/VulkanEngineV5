module;

export module VulkanBackend.Runtime.FrameLoop;

import std;

export namespace VulkanEngine::Runtime {

enum class RuntimeStatus : std::uint8_t {
    Ok,
    ResizePending,
    SwapchainOutOfDate,
    SwapchainSuboptimal,
    Minimized,
    ShutdownRequested,
    FatalError
};

struct RuntimeConfig {
    std::uint32_t frames_in_flight = 2; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct RuntimeFrameInfo {
    std::uint32_t frame_index = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t swapchain_image_index = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    RuntimeStatus status = RuntimeStatus::Ok; // NOLINT(misc-non-private-member-variables-in-classes)
};

class FrameLoop {
public:
    bool Initialize(const RuntimeConfig& config);
    void Shutdown();

    [[nodiscard]] RuntimeFrameInfo BeginFrame();
    void EndFrame();

    void NotifyWindowResized();
    void NotifySwapchainOutOfDate();
    void NotifySwapchainSuboptimal();
    void NotifyWindowMinimized(bool minimized);
    void RequestShutdown();

    [[nodiscard]] bool IsInitialized() const;
    [[nodiscard]] bool ShouldShutdown() const;

private:
    RuntimeConfig config_{};
    std::uint32_t frame_counter_ = 0;
    bool initialized_ = false;
    bool minimized_ = false;
    RuntimeStatus pending_status_ = RuntimeStatus::Ok;
};

}  // namespace VulkanEngine::Runtime
