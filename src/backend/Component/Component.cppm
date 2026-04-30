module;

#include <algorithm>
#include <future>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

export module VulkanBackend.Component;

export namespace VulkanEngine {

class Entity;

// Component type ID system
class ComponentTypeIDSystem {
private:
    inline static std::atomic_size_t next_type_id{0};

public:
    template<typename T>
    [[nodiscard]] static std::size_t GetTypeID() {
        static const std::size_t type_id = next_type_id.fetch_add(1, std::memory_order_relaxed);
        return type_id;
    }
};

// Compile-time field metadata for semi-ECS / SoA-backed component fields.
template<typename T>
struct FieldDescriptor {
    using value_type = T;

    std::string_view name{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

template<typename T>
[[nodiscard]] constexpr FieldDescriptor<T> Field(std::string_view name) noexcept {
    return FieldDescriptor<T>{name};
}

template<typename T>
[[nodiscard]] constexpr FieldDescriptor<T> field(std::string_view name) noexcept { // NOLINT(readability-identifier-naming)
    return Field<T>(name);
}

template<typename... Fields>
struct FieldList {
    using tuple_type = std::tuple<Fields...>;

    static constexpr std::size_t size = sizeof...(Fields);

    using field_tuple_type = tuple_type;

    constexpr FieldList() = default;

    constexpr explicit FieldList(Fields... descriptors)
        : fields_(std::move(descriptors)...) {}

    template<std::size_t Index>
    using descriptor_type = std::tuple_element_t<Index, tuple_type>;

    template<std::size_t Index>
    using value_type = descriptor_type<Index>::value_type;

    template<std::size_t Index>
    [[nodiscard]] constexpr descriptor_type<Index>& Get() & noexcept {
        return std::get<Index>(fields_);
    }

    template<std::size_t Index>
    [[nodiscard]] constexpr const descriptor_type<Index>& Get() const & noexcept {
        return std::get<Index>(fields_);
    }

private:
    tuple_type fields_{};
};

template<typename... Fields>
[[nodiscard]] constexpr auto MakeFields(Fields... descriptors) noexcept {
    return FieldList<Fields...>{std::move(descriptors)...};
}

template<typename... Fields>
[[nodiscard]] constexpr auto make_fields(Fields... descriptors) noexcept { // NOLINT(readability-identifier-naming)
    return MakeFields(std::move(descriptors)...);
}

// Lightweight direct-access handle to SoA storage.
// This is intentionally pointer-like so `component.position = value;` stays cheap.
template<typename T>
class FieldHandle {
public:
    using value_type = T;

private:
    value_type* ptr_ = nullptr;

public:
    constexpr FieldHandle() noexcept = default;

    constexpr explicit FieldHandle(value_type& value) noexcept
        : ptr_(std::addressof(value)) {}

    constexpr explicit FieldHandle(value_type* value) noexcept
        : ptr_(value) {}

    constexpr void Bind(value_type& value) noexcept {
        ptr_ = std::addressof(value);
    }

    constexpr void Bind(value_type* value) noexcept {
        ptr_ = value;
    }

    constexpr void Reset() noexcept {
        ptr_ = nullptr;
    }

    [[nodiscard]] constexpr value_type* Get() noexcept {
        return ptr_;
    }

    [[nodiscard]] constexpr const value_type* Get() const noexcept {
        return ptr_;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return ptr_ != nullptr;
    }

    [[nodiscard]] constexpr value_type& operator*() noexcept {
        return *ptr_;
    }

    [[nodiscard]] constexpr const value_type& operator*() const noexcept {
        return *ptr_;
    }

    [[nodiscard]] constexpr value_type* operator->() noexcept {
        return ptr_;
    }

    [[nodiscard]] constexpr const value_type* operator->() const noexcept {
        return ptr_;
    }

    constexpr operator value_type&() noexcept {
        return *ptr_;
    }

    constexpr operator const value_type&() const noexcept {
        return *ptr_;
    }

    template<typename U>
    requires std::is_assignable_v<value_type&, U&&>
    constexpr FieldHandle& operator=(U&& value) {
        *ptr_ = std::forward<U>(value);
        return *this;
    }
};

// Packed field storage for SoA-backed component fields.
// Swap-delete keeps the data compact and updates the bound handles.
template<typename T>
class PackedFieldStorage {
public:
    using value_type = T;
    using size_type = std::size_t;

    [[nodiscard]] size_type Size() const noexcept {
        return values_.size();
    }

    [[nodiscard]] bool Empty() const noexcept {
        return values_.empty();
    }

    void Reserve(size_type capacity) {
        values_.reserve(capacity);
        bindings_.reserve(capacity);
    }

    [[nodiscard]] value_type& operator[](size_type index) noexcept {
        return values_[index];
    }

    [[nodiscard]] const value_type& operator[](size_type index) const noexcept {
        return values_[index];
    }

    template<typename... Args>
    value_type& Emplace(FieldHandle<value_type>& handle, Args&&... args) {
        const auto* previous_data = values_.data();
        values_.emplace_back(std::forward<Args>(args)...);
        bindings_.push_back(&handle);

        if (values_.data() != previous_data) {
            RebindAll();
        } else {
            handle.Bind(values_.back());
        }

        return values_.back();
    }

    void RemoveSwapDelete(size_type index) {
        if (index >= values_.size()) {
            throw std::out_of_range("PackedFieldStorage::RemoveSwapDelete index out of range");
        }

        const size_type last = values_.size() - 1;
        auto* removed_handle = bindings_[index];

        if (index != last) {
            std::swap(values_[index], values_[last]);
            std::swap(bindings_[index], bindings_[last]);

            if (bindings_[index] != nullptr) {
                bindings_[index]->Bind(values_[index]);
            }
        }

        if (removed_handle != nullptr) {
            removed_handle->Reset();
        }

        values_.pop_back();
        bindings_.pop_back();
    }

    void Clear() noexcept {
        for (auto* handle : bindings_) {
            if (handle != nullptr) {
                handle->Reset();
            }
        }

        values_.clear();
        bindings_.clear();
    }

private:
    void RebindAll() noexcept {
        for (size_type i = 0; i < bindings_.size(); ++i) {
            if (bindings_[i] != nullptr) {
                bindings_[i]->Bind(values_[i]);
            }
        }
    }

    std::vector<value_type> values_;
    std::vector<FieldHandle<value_type>*> bindings_;
};

class Component {
protected:
    Entity* owner_ = nullptr; //NOLINT(misc-non-private-member-variables-in-classes)

public:
    virtual ~Component() = default;

    virtual void Initialize() {}
    virtual void Update(float delta_time) {}
    virtual void Render() {}

    void SetOwner(Entity* entity) { owner_ = entity; }
    [[nodiscard]] Entity* GetOwner() const { return owner_; }
};

class Entity {
public:
    using ComponentMap = std::unordered_map<std::size_t, Component*>;
    using EntityId = std::size_t;

    explicit Entity(EntityId id) noexcept
        : id_(id) {}

    [[nodiscard]] EntityId GetId() const noexcept {
        return id_;
    }

    template<typename T>
    [[nodiscard]] T* GetComponent() const {
        const auto it = components_.find(ComponentTypeIDSystem::GetTypeID<T>());
        if (it == components_.end()) {
            return nullptr;
        }
        return static_cast<T*>(it->second);
    }

    template<typename T>
    [[nodiscard]] bool HasComponent() const {
        return GetComponent<T>() != nullptr;
    }

private:
    friend class ComponentRegistry;

    void AttachComponent(std::size_t type_id, Component& component) {
        components_[type_id] = &component;
    }

    void DetachComponent(std::size_t type_id) {
        components_.erase(type_id);
    }

    EntityId id_ = 0;
    ComponentMap components_{};
};

class ComponentRegistry {
private:
    struct IComponentPool {
        virtual ~IComponentPool() = default;
        virtual void InitializeAll() = 0;
        virtual void CollectAll(std::vector<Component*>& out_components) = 0;
        virtual void Clear() = 0;
    };

    template<typename T>
    class ComponentPool final : public IComponentPool {
    public:
        template<typename... Args>
        T& Emplace(Entity& owner, Args&&... args) {
            if (owner.HasComponent<T>()) {
                throw std::logic_error("Entity already owns this component type");
            }

            auto component = std::make_unique<T>(std::forward<Args>(args)...);
            component->SetOwner(&owner);
            auto& component_ref = *component;
            entries_.push_back(Entry{&owner, std::move(component)});
            owner.AttachComponent(TypeId(), component_ref);
            return component_ref;
        }

        template<typename Fn>
        void ForEach(Fn&& fn) {
            for (auto& entry : entries_) {
                if (entry.component) {
                    fn(*entry.component);
                }
            }
        }

        [[nodiscard]] std::vector<T*> GetAll() {
            std::vector<T*> out{};
            out.reserve(entries_.size());
            for (auto& entry : entries_) {
                if (entry.component) {
                    out.push_back(entry.component.get());
                }
            }
            return out;
        }

        void InitializeAll() override {
            for (auto& entry : entries_) {
                if (entry.component) {
                    entry.component->Initialize();
                }
            }
        }

        void CollectAll(std::vector<Component*>& out_components) override {
            out_components.reserve(out_components.size() + entries_.size());
            for (auto& entry : entries_) {
                if (entry.component) {
                    out_components.push_back(entry.component.get());
                }
            }
        }

        void Clear() override {
            for (auto& entry : entries_) {
                if (entry.owner != nullptr) {
                    entry.owner->DetachComponent(TypeId());
                }
                if (entry.component) {
                    entry.component->SetOwner(nullptr);
                }
            }
            entries_.clear();
        }

    private:
        struct Entry {
            Entity* owner = nullptr;
            std::unique_ptr<T> component{};
        };

        [[nodiscard]] static std::size_t TypeId() {
            static const std::size_t type_id = ComponentTypeIDSystem::GetTypeID<T>();
            return type_id;
        }

        std::vector<Entry> entries_{};
    };

    template<typename T>
    [[nodiscard]] ComponentPool<T>* GetPool() {
        const auto type_id = ComponentTypeIDSystem::GetTypeID<T>();
        const auto it = pools_.find(type_id);
        if (it == pools_.end()) {
            return nullptr;
        }
        return static_cast<ComponentPool<T>*>(it->second.get());
    }

    template<typename T>
    [[nodiscard]] ComponentPool<T>& GetOrCreatePool() {
        const auto type_id = ComponentTypeIDSystem::GetTypeID<T>();
        const auto it = pools_.find(type_id);
        if (it != pools_.end()) {
            return *static_cast<ComponentPool<T>*>(it->second.get());
        }

        auto pool = std::make_unique<ComponentPool<T>>();
        auto& pool_ref = *pool;
        pools_[type_id] = std::move(pool);
        return pool_ref;
    }

    [[nodiscard]] std::vector<Component*> GatherAllComponents() {
        std::vector<Component*> components{};
        for (auto& [_, pool] : pools_) {
            pool->CollectAll(components);
        }
        return components;
    }

    std::unordered_map<std::size_t, std::unique_ptr<IComponentPool>> pools_{};
    std::vector<std::unique_ptr<Entity>> entities_{};
    Entity::EntityId next_entity_id_ = 0;
    mutable std::mutex mutex_{};

public:
    [[nodiscard]] Entity& CreateEntity() {
        const std::scoped_lock lock(mutex_);
        entities_.push_back(std::make_unique<Entity>(next_entity_id_++));
        return *entities_.back();
    }

    template<typename T, typename... Args>
    T& AddComponent(Entity& owner, Args&&... args) {
        const std::scoped_lock lock(mutex_);
        auto& pool = GetOrCreatePool<T>();
        return pool.Emplace(owner, std::forward<Args>(args)...);
    }

    template<typename T>
    [[nodiscard]] std::vector<T*> GetAll() {
        const std::scoped_lock lock(mutex_);
        auto* pool = GetPool<T>();
        return pool != nullptr ? pool->GetAll() : std::vector<T*>{};
    }

    template<typename T, typename Fn>
    void ForEach(Fn&& fn) {
        const std::scoped_lock lock(mutex_);
        auto* pool = GetPool<T>();
        if (pool != nullptr) {
            pool->ForEach(std::forward<Fn>(fn));
        }
    }

    void InitializeAllComponents() {
        const std::scoped_lock lock(mutex_);
        for (auto& [_, pool] : pools_) {
            pool->InitializeAll();
        }
    }

    void UpdateAllComponentsAsync(float delta_time) {
        std::vector<Component*> components;
        {
            const std::scoped_lock lock(mutex_);
            components = GatherAllComponents();
        }

        if (components.empty()) {
            return;
        }

        const unsigned int worker_count = std::max(1u, std::thread::hardware_concurrency());
        const std::size_t chunk_size = std::max<std::size_t>(1, (components.size() + worker_count - 1) / worker_count);

        std::vector<std::future<void>> tasks;
        tasks.reserve((components.size() + chunk_size - 1) / chunk_size);

        for (std::size_t start = 0; start < components.size(); start += chunk_size) {
            const std::size_t end = std::min(components.size(), start + chunk_size);
            tasks.emplace_back(std::async(std::launch::async, [start, end, delta_time, components]() mutable {
                for (std::size_t index = start; index < end; ++index) {
                    if (components[index] != nullptr) {
                        components[index]->Update(delta_time);
                    }
                }
            }));
        }

        for (auto& task : tasks) {
            task.get();
        }
    }

    void Clear() {
        const std::scoped_lock lock(mutex_);
        for (auto& [_, pool] : pools_) {
            pool->Clear();
        }
        pools_.clear();
        entities_.clear();
    }
};

} // namespace VulkanEngine
