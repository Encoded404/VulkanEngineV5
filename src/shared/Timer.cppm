module;

export module VulkanShared.Timer;

import std;

export namespace VulkanShared {

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
    [[nodiscard]] std::int64_t ElapsedNsInt() const noexcept;

    [[nodiscard]] bool IsRunning() const noexcept;

private:
    std::int64_t accumulated_;
    std::int64_t start_time_;
    bool running_;

    [[nodiscard]] std::int64_t ElapsedRaw() const noexcept;
    static std::int64_t Now() noexcept;
};

} // namespace VulkanEngine::Utils
