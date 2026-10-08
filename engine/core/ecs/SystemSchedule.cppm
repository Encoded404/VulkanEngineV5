// Ordered data-system schedule for the hybrid ECS.
//
// Object components drive themselves through Component::Update. Data
// components do not: systems read them through registry views. Without an
// explicit order, that second path had no home in the frame. This schedule is
// deliberately minimal — an ordered list that runs once per fixed step, which is
// enough to make system order a decision instead of an accident.

module;

export module VulkanEngine.ECS.SystemSchedule;

import std;

export import VulkanEngine.ECS.ComponentRegistry;

export namespace VulkanEngine {

class SystemSchedule {
public:
    using SystemFn = std::function<void(ComponentRegistry& registry, float delta_time)>;

    // Registration order is execution order.
    void Add(std::string name, SystemFn fn) {
        systems_.push_back(Entry{std::move(name), std::move(fn)});
    }

    void Run(ComponentRegistry& registry, float delta_time) const {
        for (const Entry& system : systems_) {
            system.fn(registry, delta_time);
        }
    }

    [[nodiscard]] std::size_t Size() const noexcept { return systems_.size(); }

    void Clear() noexcept { systems_.clear(); }

private:
    struct Entry {
        std::string name;
        SystemFn fn;
    };

    std::vector<Entry> systems_{};
};

} // namespace VulkanEngine
