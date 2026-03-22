#ifndef VULKAN_ENGINE_COMPONENT_COMPONENT_HPP
#define VULKAN_ENGINE_COMPONENT_COMPONENT_HPP

// Forward declarations
class Entity;

// Base component class
class Component {
protected:
    Entity* owner = nullptr;

public:
    virtual ~Component() = default;

    virtual void Initialize() {}
    virtual void Update(float deltaTime) {}
    virtual void Render() {}

    void SetOwner(Entity* entity) { owner = entity; }
    Entity* GetOwner() const { return owner; }
};

#endif // VULKAN_ENGINE_COMPONENT_COMPONENT_HPP