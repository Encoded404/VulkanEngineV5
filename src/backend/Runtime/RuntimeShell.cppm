module;

#include <cstdint>

export module VulkanEngine.Runtime.RuntimeShell;

export namespace VulkanEngine::Runtime {

enum class RuntimeStatus : uint8_t {
    Ok,
    ResizePending,
    SwapchainOutOfDate,
    SwapchainSuboptimal,
    Minimized,
    ShutdownRequested,
    FatalError
};

struct RuntimeConfig {
    uint32_t frames_in_flight = 2; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct RuntimeFrameInfo {
    uint32_t frame_index = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t swapchain_image_index = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    RuntimeStatus status = RuntimeStatus::Ok; // NOLINT(misc-non-private-member-variables-in-classes)
};

class RuntimeShell {
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
    uint32_t frame_counter_ = 0;
    bool initialized_ = false;
    bool minimized_ = false;
    RuntimeStatus pending_status_ = RuntimeStatus::Ok;
};

}  // namespace VulkanEngine::Runtime
