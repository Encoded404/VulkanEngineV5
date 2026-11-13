// Component context: the narrow service view granted to object components
// (rung 3 of engine modularization).
//
// Object components need engine services (input, physics, config), but
// construction-site injection forces games into per-type wiring — every factory
// repeats the engine context it collected. The fix keeps the ECS contract
// intact: the engine builds ONE view of the services an object component is
// allowed to see and hands it to Component::Update. The component never reaches
// the world or the engine; it reaches what was granted.
//
// The grants are private: the engine and tests set them with GrantInput /
// GrantPhysicsWorld, and a component reads them with the accessors. That closes
// the direct-field bypass — a component cannot reach a service it did not
// declare, and the accessors report a violation when it tries during an update
// (see ComponentAccess). Outside an update (setup, systems, tests) the
// accessors return the grant unvalidated: the check guards the barriered path,
// it is not a capability system for arbitrary calls.
//
// A write grant satisfies a read, so a declared writer may call InputRead()
// while a declared reader calling Input() is rejected. Readers do not serialize
// against other readers.

module;

#include <cassert>

export module VulkanEngine.ECS.ComponentContext;

import std;

import VulkanEngine.ECS.ComponentAccess;

import VulkanEngine.Input;

#ifdef VKENGINE_PHYSICS
import VulkanEngine.Physics.World;
#endif

export namespace VulkanEngine {

struct ComponentContext {
    // Input bindings and edge/level queries. The write grant: binding an action
    // mutates the input system, so it belongs in OnConfigure, never in Update.
    [[nodiscard]] VulkanEngine::Input::InputSystem* Input() const {
        EnforceServiceAccess(Service::Input, true);
        return input_;
    }
    // The const read grant: queries only, safe to share with another reader.
    [[nodiscard]] const VulkanEngine::Input::InputSystem* InputRead() const {
        EnforceServiceAccess(Service::Input, false);
        return input_;
    }

#ifdef VKENGINE_PHYSICS
    // The physics world facade, for steering/diagnosing components that need it
    // (queries, velocity control). Granted when the physics module is running;
    // null otherwise.
    [[nodiscard]] VulkanEngine::Physics::World* PhysicsWorld() const {
        EnforceServiceAccess(Service::PhysicsWorld, true);
        return physics_world_;
    }
    [[nodiscard]] const VulkanEngine::Physics::World* PhysicsWorldRead() const {
        EnforceServiceAccess(Service::PhysicsWorld, false);
        return physics_world_;
    }
#endif

    void GrantInput(VulkanEngine::Input::InputSystem* input) noexcept { input_ = input; }
#ifdef VKENGINE_PHYSICS
    void GrantPhysicsWorld(VulkanEngine::Physics::World* world) noexcept {
        physics_world_ = world;
    }
#endif

private:
    VulkanEngine::Input::InputSystem* input_ = nullptr;
#ifdef VKENGINE_PHYSICS
    VulkanEngine::Physics::World* physics_world_ = nullptr;
#endif
};

} // namespace VulkanEngine
