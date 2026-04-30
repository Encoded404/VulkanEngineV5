module;

export module VulkanEngine.Components.MeshRenderer;

import VulkanBackend.Component;
import VulkanEngine.Components.Transform;

export namespace App::Components {

class MeshRenderer : public VulkanEngine::Component {
public:
    bool visible = true; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] App::Components::Transform* GetTransform() const {
        return GetOwner() != nullptr ? GetOwner()->GetComponent<App::Components::Transform>() : nullptr;
    }

    static auto GetFields() {
        return VulkanEngine::make_fields(
            VulkanEngine::field<bool>("visible")
        );
    }
};

} // namespace App::Components


