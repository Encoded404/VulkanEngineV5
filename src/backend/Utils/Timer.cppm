module;

#include <cstdint>

export module VulkanBackend.Utils.Timer;

export namespace VulkanEngine::Utils {

class Timer {
public:
    Timer() noexcept;
    explicit Timer(bool start) noexcept;

    void Start() noexcept;
    void Stop() noexcept;
    void Restart() noexcept;

    [[nodiscard]] double ElapsedMs() const noexcept;
    [[nodiscard]] double ElapsedUs() const noexcept;
    [[nodiscard]] double ElapsedNs() const noexcept;
    [[nodiscard]] double ElapsedS() const noexcept;
    [[nodiscard]] int64_t ElapsedNsInt() const noexcept;

    [[nodiscard]] bool IsRunning() const noexcept;

private:
    int64_t accumulated_;
    int64_t start_time_;
    bool running_;

    [[nodiscard]] int64_t ElapsedRaw() const noexcept;
    static int64_t Now() noexcept;
};

} // namespace VulkanEngine::Utils
