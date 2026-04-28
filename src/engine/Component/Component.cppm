module;

#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module VulkanEngine.Component;

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

} // namespace VulkanEngine
