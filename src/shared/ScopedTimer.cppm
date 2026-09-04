module;

export module VulkanShared.ScopedSection;

import std;

import VulkanShared.Timer;

export namespace VulkanShared {

using SectionSink = std::function<void(const std::string& name, double ms)>;

class ScopedSection {
public:
    ScopedSection(std::string name, SectionSink sink = {})
        : name_(std::move(name))
        , sink_(std::move(sink))
        , timer_{true} {}

    ScopedSection(const ScopedSection&) = delete;
    ScopedSection& operator=(const ScopedSection&) = delete;

    ScopedSection(ScopedSection&&) noexcept = default;
    ScopedSection& operator=(ScopedSection&&) noexcept = default;

    ~ScopedSection() {
        if (sink_) {
            sink_(name_, timer_.ElapsedMs());
        }
    }

private:
    std::string name_;
    SectionSink sink_;
    Timer timer_;
};

} // namespace VulkanShared