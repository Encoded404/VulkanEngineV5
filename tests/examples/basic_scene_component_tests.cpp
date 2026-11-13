// The basic scene's object components, exercised without a device.
//
// The example itself needs a window and a swapchain, so a component regression
// there would only be caught by launching it. This builds the same components
// against a registry and a bare InputSystem instead: the controller must bind its
// actions exactly once (in OnConfigure, not on the parallel path), the transform
// it drives must move through the scope, and the declarations it carries must
// satisfy the strict validator.

#include <gtest/gtest.h>

#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)
#include <glm/gtc/quaternion.hpp> // NOLINT(misc-include-cleaner)

import std;

import VulkanEngine.ECS.ComponentContext;
import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.Components.Transform;
import VulkanEngine.Input;

import Examples.BasicScene.Components.SimpleControllerComponent;
import Examples.BasicScene.Components.TransformControlComponent;

using namespace VulkanEngine;
using Examples::BasicScene::Components::SimpleControllerComponent;
using Examples::BasicScene::Components::TransformControlComponent;

TEST(BasicSceneComponentTest, TheControllerBindsOnceAndRotatesItsTransform) {
    ComponentRegistry registry;
    // Debug builds compile the validator in; assert the declarations are right
    // by running the strict policy and observing no throw.
    registry.SetAccessOptions(
        ComponentAccessOptions{.checks = true, .on_violation = ViolationAction::Throw});

    Input::InputSystem input{};
    ComponentContext context{};
    context.GrantInput(&input);

    Entity& entity = registry.CreateEntity();
    auto& transform = registry.AddComponent<Components::Transform>(entity);
    transform.position = glm::vec3{0.0f};
    transform.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    registry.AddComponent<SimpleControllerComponent>(entity);

    constexpr float kDelta = 0.5f;
    registry.UpdateComponents(kDelta, context);

    // The bindings belong to OnConfigure: "move" plus "pause_spin", each bound
    // once. A second pass must not bind anything again.
    EXPECT_EQ(input.GetAllActions().size(), 2u);
    registry.UpdateComponents(kDelta, context);
    EXPECT_EQ(input.GetAllActions().size(), 2u) << "OnConfigure must run once per instance";

    // No keys are held, so the transform only spins (90 deg/s at 0.5 s per pass).
    EXPECT_NEAR(transform.position.x, 0.0f, 1e-5f);
    EXPECT_NEAR(transform.position.y, 0.0f, 1e-5f);
    const float angle = glm::angle(transform.rotation);
    EXPECT_NEAR(glm::degrees(angle), 90.0f, 1.0f) << "two half-second passes at 90 deg/s";
}

TEST(BasicSceneComponentTest, TheControllerIsSelfParallelSafe) {
    // The declaration is the opt-in: instances on different entities are
    // disjoint, and the type declares no cross-entity access.
    constexpr auto spec = kAccessSpec<SimpleControllerComponent>;
    EXPECT_EQ(spec.concurrency, Concurrency::SelfParallel);
    EXPECT_TRUE(spec.any.empty());
    EXPECT_EQ(spec.services & ServiceWriteMask(), 0u);
    EXPECT_EQ(spec.self_writes.size(), 1u);
    EXPECT_FALSE(spec.self_reads.empty());
}

TEST(BasicSceneComponentTest, TheTransformControlWritesItsOwnTransform) {
    ComponentRegistry registry;
    registry.SetAccessOptions(
        ComponentAccessOptions{.checks = true, .on_violation = ViolationAction::Throw});

    Entity& entity = registry.CreateEntity();
    auto& transform = registry.AddComponent<Components::Transform>(entity);
    auto& control = registry.AddComponent<TransformControlComponent>(entity);
    control.position = glm::vec3{4.0f, 5.0f, 6.0f};

    registry.UpdateComponents(0.016f);

    EXPECT_NEAR(transform.position.x, 4.0f, 1e-5f);
    EXPECT_NEAR(transform.position.y, 5.0f, 1e-5f);
    EXPECT_NEAR(transform.position.z, 6.0f, 1e-5f);
}
