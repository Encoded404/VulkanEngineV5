module;

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

export module VulkanBackend.Utils.CallbackList;

export namespace VulkanEngine::Utils {

struct Handle {
    uint32_t index;
    uint32_t generation;
};

template<typename Signature>
class CallbackBase;

template<typename Signature>
class ScopedHandle {
    CallbackBase<Signature>* owner_ = nullptr;
    Handle handle_{};

public:
    ScopedHandle() = default;

    ScopedHandle(CallbackBase<Signature>* owner, Handle handle)
        : owner_(owner), handle_(handle) {}

    ~ScopedHandle() {
        if (owner_) owner_->Unregister(handle_);
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept
        : owner_(other.owner_), handle_(other.handle_) {
        other.owner_ = nullptr;
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            if (owner_) owner_->Unregister(handle_);
            owner_ = other.owner_;
            handle_ = other.handle_;
            other.owner_ = nullptr;
        }
        return *this;
    }
};

template<typename Signature>
class CallbackBase {
public:
    using Callback = std::function<Signature>;

    virtual ~CallbackBase() = default;

    ScopedHandle<Signature> Register(Callback cb) {
        return ScopedHandle<Signature>(this, RegisterRaw(std::move(cb)));
    }

    virtual Handle RegisterRaw(Callback cb) = 0;
    virtual void Unregister(Handle handle) = 0;
};

template<typename Ret, typename... Args>
class CallbackList : public CallbackBase<Ret(Args...)> {
    using typename CallbackBase<Ret(Args...)>::Callback;

    struct Entry {
        uint32_t generation;
        Callback callback;
    };

    std::vector<std::optional<Entry>> slots_;
    std::vector<uint32_t> free_indices_;
    uint32_t next_generation_ = 1;

    uint32_t AcquireSlot() {
        if (!free_indices_.empty()) {
            const uint32_t idx = free_indices_.back();
            free_indices_.pop_back();
            return idx;
        }
        const uint32_t idx = static_cast<uint32_t>(slots_.size());
        slots_.emplace_back();
        return idx;
    }

public:
    Handle RegisterRaw(Callback cb) override {
        const uint32_t idx = AcquireSlot();
        const uint32_t gen = next_generation_++;
        slots_[idx] = Entry{gen, std::move(cb)};
        return Handle{idx, gen};
    }

    void Unregister(Handle handle) override {
        if (handle.index >= slots_.size()) return;
        auto& slot = slots_[handle.index];
        if (!slot || slot->generation != handle.generation) return;
        slot.reset();
        free_indices_.push_back(handle.index);
    }

    Ret Call(Args... args) const {
        Ret result{};
        bool first = true;
        for (auto& slot : slots_) {
            if (slot) {
                if constexpr (std::is_same_v<Ret, void>) {
                    slot->callback(args...);
                } else {
                    if (first) {
                        result = slot->callback(args...);
                        first = false;
                    } else {
                        result = slot->callback(args...);
                    }
                }
            }
        }
        if constexpr (!std::is_same_v<Ret, void>) {
            return result;
        }
    }
};

template<typename... Args>
class CallbackList<void(Args...)> : public CallbackBase<void(Args...)> {
    using typename CallbackBase<void(Args...)>::Callback;

    struct Entry {
        uint32_t generation;
        Callback callback;
    };

    std::vector<std::optional<Entry>> slots_;
    std::vector<uint32_t> free_indices_;
    uint32_t next_generation_ = 1;

    uint32_t AcquireSlot() {
        if (!free_indices_.empty()) {
            const uint32_t idx = free_indices_.back();
            free_indices_.pop_back();
            return idx;
        }
        const uint32_t idx = static_cast<uint32_t>(slots_.size());
        slots_.emplace_back();
        return idx;
    }

public:
    Handle RegisterRaw(Callback cb) override {
        const uint32_t idx = AcquireSlot();
        const uint32_t gen = next_generation_++;
        slots_[idx] = Entry{gen, std::move(cb)};
        return Handle{idx, gen};
    }

    void Unregister(Handle handle) override {
        if (handle.index >= slots_.size()) return;
        auto& slot = slots_[handle.index];
        if (!slot || slot->generation != handle.generation) return;
        slot.reset();
        free_indices_.push_back(handle.index);
    }

    void Call(Args... args) const {
        for (auto& slot : slots_) {
            if (slot) {
                slot->callback(args...);
            }
        }
    }
};

template<typename... Args>
class CallbackList<bool(Args...)> : public CallbackBase<bool(Args...)> {
    using typename CallbackBase<bool(Args...)>::Callback;

    struct Entry {
        uint32_t generation;
        Callback callback;
    };

    std::vector<std::optional<Entry>> slots_;
    std::vector<uint32_t> free_indices_;
    uint32_t next_generation_ = 1;

    uint32_t AcquireSlot() {
        if (!free_indices_.empty()) {
            const uint32_t idx = free_indices_.back();
            free_indices_.pop_back();
            return idx;
        }
        const uint32_t idx = static_cast<uint32_t>(slots_.size());
        slots_.emplace_back();
        return idx;
    }

public:
    Handle RegisterRaw(Callback cb) override {
        const uint32_t idx = AcquireSlot();
        const uint32_t gen = next_generation_++;
        slots_[idx] = Entry{gen, std::move(cb)};
        return Handle{idx, gen};
    }

    void Unregister(Handle handle) override {
        if (handle.index >= slots_.size()) return;
        auto& slot = slots_[handle.index];
        if (!slot || slot->generation != handle.generation) return;
        slot.reset();
        free_indices_.push_back(handle.index);
    }

    bool Call(Args... args) const {
        for (const auto& slot : slots_) {
            if (slot) {
                if (!slot->callback(args...)) {
                    return false;
                }
            }
        }
        return true;
    }
};

template<typename Ret, typename... Args>
class OrderedCallbackList : public CallbackBase<Ret(Args...)> {
    using typename CallbackBase<Ret(Args...)>::Callback;

    struct Entry {
        uint32_t generation;
        Callback callback;
        int priority;
    };

    std::vector<std::optional<Entry>> slots_;
    std::vector<uint32_t> order_;
    std::vector<uint32_t> free_indices_;
    uint32_t next_generation_ = 1;

    uint32_t AcquireSlot() {
        if (!free_indices_.empty()) {
            const uint32_t idx = free_indices_.back();
            free_indices_.pop_back();
            return idx;
        }
        const uint32_t idx = static_cast<uint32_t>(slots_.size());
        slots_.emplace_back();
        return idx;
    }

    void InsertInOrder(uint32_t slot_idx, int priority) {
        auto it = order_.begin();
        while (it != order_.end()) {
            auto& slot = slots_[*it];
            if (slot && slot->priority > priority) break;
            ++it;
        }
        order_.insert(it, slot_idx);
    }

public:
    ScopedHandle<Ret(Args...)> Register(Callback cb, int priority = 0) {
        return ScopedHandle<Ret(Args...)>(this, RegisterRawWithPriority(std::move(cb), priority));
    }

    Handle RegisterRaw(Callback cb) override {
        return RegisterRawWithPriority(std::move(cb), 0);
    }

    Handle RegisterRawWithPriority(Callback cb, int priority) {
        const uint32_t idx = AcquireSlot();
        const uint32_t gen = next_generation_++;
        slots_[idx] = Entry{gen, std::move(cb), priority};
        InsertInOrder(idx, priority);
        return Handle{idx, gen};
    }

    void Unregister(Handle handle) override {
        if (handle.index >= slots_.size()) return;
        auto& slot = slots_[handle.index];
        if (!slot || slot->generation != handle.generation) return;
        slot.reset();
        free_indices_.push_back(handle.index);
        std::erase(order_, handle.index);
    }

    Ret Call(Args... args) const {
        Ret result{};
        bool first = true;
        for (const uint32_t idx : order_) {
            const auto& slot = slots_[idx];
            if (slot) {
                if constexpr (std::is_same_v<Ret, void>) {
                    slot->callback(args...);
                } else {
                    if (first) {
                        result = slot->callback(args...);
                        first = false;
                    } else {
                        result = slot->callback(args...);
                    }
                }
            }
        }
        if constexpr (!std::is_same_v<Ret, void>) {
            return result;
        }
    }
};

template<typename... Args>
class OrderedCallbackList<void(Args...)> : public CallbackBase<void(Args...)> {
    using typename CallbackBase<void(Args...)>::Callback;

    struct Entry {
        uint32_t generation;
        Callback callback;
        int priority;
    };

    std::vector<std::optional<Entry>> slots_;
    std::vector<uint32_t> order_;
    std::vector<uint32_t> free_indices_;
    uint32_t next_generation_ = 1;

    uint32_t AcquireSlot() {
        if (!free_indices_.empty()) {
            const uint32_t idx = free_indices_.back();
            free_indices_.pop_back();
            return idx;
        }
        const uint32_t idx = static_cast<uint32_t>(slots_.size());
        slots_.emplace_back();
        return idx;
    }

    void InsertInOrder(uint32_t slot_idx, int priority) {
        auto it = order_.begin();
        while (it != order_.end()) {
            auto& slot = slots_[*it];
            if (slot && slot->priority > priority) break;
            ++it;
        }
        order_.insert(it, slot_idx);
    }

public:
    ScopedHandle<void(Args...)> Register(Callback cb, int priority = 0) {
        return ScopedHandle<void(Args...)>(this, RegisterRawWithPriority(std::move(cb), priority));
    }

    Handle RegisterRaw(Callback cb) override {
        return RegisterRawWithPriority(std::move(cb), 0);
    }

    Handle RegisterRawWithPriority(Callback cb, int priority) {
        const uint32_t idx = AcquireSlot();
        const uint32_t gen = next_generation_++;
        slots_[idx] = Entry{gen, std::move(cb), priority};
        InsertInOrder(idx, priority);
        return Handle{idx, gen};
    }

    void Unregister(Handle handle) override {
        if (handle.index >= slots_.size()) return;
        auto& slot = slots_[handle.index];
        if (!slot || slot->generation != handle.generation) return;
        slot.reset();
        free_indices_.push_back(handle.index);
        std::erase(order_, handle.index);
    }

    void Call(Args... args) const {
        for (const uint32_t idx : order_) {
            const auto& slot = slots_[idx];
            if (slot) {
                slot->callback(args...);
            }
        }
    }
};

template<typename... Args>
class OrderedCallbackList<bool(Args...)> : public CallbackBase<bool(Args...)> {
    using typename CallbackBase<bool(Args...)>::Callback;

    struct Entry {
        uint32_t generation;
        Callback callback;
        int priority;
    };

    std::vector<std::optional<Entry>> slots_;
    std::vector<uint32_t> order_;
    std::vector<uint32_t> free_indices_;
    uint32_t next_generation_ = 1;

    uint32_t AcquireSlot() {
        if (!free_indices_.empty()) {
            const uint32_t idx = free_indices_.back();
            free_indices_.pop_back();
            return idx;
        }
        const uint32_t idx = static_cast<uint32_t>(slots_.size());
        slots_.emplace_back();
        return idx;
    }

    void InsertInOrder(uint32_t slot_idx, int priority) {
        auto it = order_.begin();
        while (it != order_.end()) {
            auto& slot = slots_[*it];
            if (slot && slot->priority > priority) break;
            ++it;
        }
        order_.insert(it, slot_idx);
    }

public:
    ScopedHandle<bool(Args...)> Register(Callback cb, int priority = 0) {
        return ScopedHandle<bool(Args...)>(this, RegisterRawWithPriority(std::move(cb), priority));
    }

    Handle RegisterRaw(Callback cb) override {
        return RegisterRawWithPriority(std::move(cb), 0);
    }

    Handle RegisterRawWithPriority(Callback cb, int priority) {
        const uint32_t idx = AcquireSlot();
        const uint32_t gen = next_generation_++;
        slots_[idx] = Entry{gen, std::move(cb), priority};
        InsertInOrder(idx, priority);
        return Handle{idx, gen};
    }

    void Unregister(Handle handle) override {
        if (handle.index >= slots_.size()) return;
        auto& slot = slots_[handle.index];
        if (!slot || slot->generation != handle.generation) return;
        slot.reset();
        free_indices_.push_back(handle.index);
        std::erase(order_, handle.index);
    }

    bool Call(Args... args) const {
        for (const uint32_t idx : order_) {
            const auto& slot = slots_[idx];
            if (slot) {
                if (!slot->callback(args...)) {
                    return false;
                }
            }
        }
        return true;
    }
};

}
