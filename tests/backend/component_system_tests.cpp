#include <gtest/gtest.h>

#include <logging/logging.hpp>

import VulkanBackend.Component;

#include <type_traits>

#include "../test_logging.hpp"

namespace {
// Compile-time schema used to verify field metadata extraction in the tests.
struct TransformSchema {
    static constexpr auto fields = VulkanEngine::make_fields(
        VulkanEngine::field<int>("position"),
        VulkanEngine::field<float>("rotation")
    );
};

struct RegistryProbeComponent : VulkanEngine::Component {
public:
    [[nodiscard]] bool WasInitialized() const noexcept { return initialized_; }
    [[nodiscard]] bool WasUpdated() const noexcept { return updated_; }
    [[nodiscard]] float GetLastDeltaTime() const noexcept { return last_delta_time_; }

    void Initialize() override {
        initialized_ = true;
    }

    void Update(float delta_time) override {
        updated_ = true;
        last_delta_time_ = delta_time;
    }

private:
    bool initialized_ = false;
    bool updated_ = false;
    float last_delta_time_ = 0.0f;
};
}  // namespace

TEST(ComponentSystemTest, FieldMetadataPreservesNamesAndTypes) {
    TestLogging::InstallPerTestFileLogger();

    // Capture the schema's field list as a constexpr value for type checks.
    constexpr auto fields = TransformSchema::fields;

    // The schema should expose exactly two fields.
    static_assert(decltype(fields)::size == 2);

    // Extract the field wrapper types so we can verify their declared value types.
    using PositionField = std::remove_cvref_t<decltype(fields.Get<0>())>;
    using RotationField = std::remove_cvref_t<decltype(fields.Get<1>())>;

    // Each field wrapper should preserve the underlying field type.
    static_assert(std::is_same_v<PositionField::value_type, int>);
    static_assert(std::is_same_v<RotationField::value_type, float>);

    LOGIFACE_LOG(info, "Validating field metadata for TransformSchema");

    // Field names should match the schema definition.
    EXPECT_EQ(fields.Get<0>().name, "position");
    EXPECT_EQ(fields.Get<1>().name, "rotation");
}

TEST(ComponentSystemTest, FieldHandleActsLikePointerBackedAccess) {
    TestLogging::InstallPerTestFileLogger();

    // Start with a plain value and bind a handle to it.
    int position = 7;
    VulkanEngine::FieldHandle<int> handle(position);

    LOGIFACE_LOG(info, "Checking FieldHandle pointer-like access");

    // A live handle should point at the original value and dereference like a pointer.
    ASSERT_NE(handle.Get(), nullptr);
    EXPECT_EQ(handle.Get(), &position);
    EXPECT_EQ(*handle, 7);

    // Assigning through the handle should update the referenced value.
    handle = 11;
    EXPECT_EQ(position, 11);

    // Resetting should detach the handle from the value.
    handle.Reset();
    EXPECT_EQ(handle.Get(), nullptr);
}

TEST(ComponentSystemTest, PackedFieldStorageSwapDeleteRebindsHandles) {
    TestLogging::InstallPerTestFileLogger();

    // Prepare storage and reserve room for two elements.
    VulkanEngine::PackedFieldStorage<int> storage;
    storage.Reserve(2);

    // Keep handles to both stored values so we can verify rebinding later.
    VulkanEngine::FieldHandle<int> first;
    VulkanEngine::FieldHandle<int> second;

    LOGIFACE_LOG(info, "Emplacing values into PackedFieldStorage");

    // Emplace two values and capture their handles.
    storage.Emplace(first, 10);
    storage.Emplace(second, 20);

    // Both handles should resolve to the inserted values.
    ASSERT_NE(first.Get(), nullptr);
    ASSERT_NE(second.Get(), nullptr);
    EXPECT_EQ(*first, 10);
    EXPECT_EQ(*second, 20);

    LOGIFACE_LOG(info, "Removing index 0 with swap-delete");

    // Removing one item should invalidate its handle and preserve the other.
    storage.RemoveSwapDelete(0);

    EXPECT_EQ(first.Get(), nullptr);
    ASSERT_NE(second.Get(), nullptr);
    EXPECT_EQ(storage.Size(), 1u);
    EXPECT_EQ(storage[0], 20);
    EXPECT_EQ(*second, 20);
}

TEST(ComponentSystemTest, TypeIdSystemReturnsStableIdsPerType) {
    TestLogging::InstallPerTestFileLogger();

    // Two distinct types should receive distinct stable IDs.
    struct TypeA {};
    struct TypeB {};

    LOGIFACE_LOG(info, "Checking stable component type ids");

    // Repeated requests for the same type must return the same ID.
    const auto id_a_1 = VulkanEngine::ComponentTypeIDSystem::GetTypeID<TypeA>();
    const auto id_a_2 = VulkanEngine::ComponentTypeIDSystem::GetTypeID<TypeA>();
    const auto id_b = VulkanEngine::ComponentTypeIDSystem::GetTypeID<TypeB>();

    EXPECT_EQ(id_a_1, id_a_2);
    EXPECT_NE(id_a_1, id_b);
}

TEST(ComponentSystemTest, RegistryStoresComponentsAndEntityReferences) {
    TestLogging::InstallPerTestFileLogger();

    VulkanEngine::ComponentRegistry registry;
    auto& entity = registry.CreateEntity();
    auto& component = registry.AddComponent<RegistryProbeComponent>(entity);

    EXPECT_TRUE(entity.HasComponent<RegistryProbeComponent>());
    EXPECT_EQ(entity.GetComponent<RegistryProbeComponent>(), &component);

    const auto all_components = registry.GetAll<RegistryProbeComponent>();
    ASSERT_EQ(all_components.size(), 1u);
    EXPECT_EQ(all_components[0], &component);

    bool visited = false;
    registry.ForEach<RegistryProbeComponent>([&](RegistryProbeComponent& probe) {
        visited = true;
        EXPECT_EQ(&probe, &component);
    });
    EXPECT_TRUE(visited);
}

TEST(ComponentSystemTest, RegistryInitializesAndUpdatesComponentsAsync) {
    TestLogging::InstallPerTestFileLogger();

    VulkanEngine::ComponentRegistry registry;
    auto& entity = registry.CreateEntity();
    auto& component = registry.AddComponent<RegistryProbeComponent>(entity);

    registry.InitializeAllComponents();
    EXPECT_TRUE(component.WasInitialized());

    registry.UpdateAllComponentsAsync(0.25f);
    EXPECT_TRUE(component.WasUpdated());
    EXPECT_FLOAT_EQ(component.GetLastDeltaTime(), 0.25f);
}

