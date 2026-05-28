module;

export module VulkanEngine.Components.MeshRenderer;

import VulkanBackend.Component;
import VulkanEngine.Components.Transform;

export namespace VulkanEngine::Components {

class MeshRenderer : public VulkanEngine::Component {
public:
    bool visible = true; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] VulkanEngine::Components::Transform* GetTransform() const {
        return GetOwner() != nullptr ? GetOwner()->GetComponent<VulkanEngine::Components::Transform>() : nullptr;
    }

    static auto GetFields() {
        return VulkanEngine::make_fields(
            VulkanEngine::field<bool>("visible")
        );
    }
};

} // namespace VulkanEngine::Components


