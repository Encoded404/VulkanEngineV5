module;

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/quaternion.hpp> //NOLINT(misc-include-cleaner)

export module App.Components.TransformControlComponent;

import std;

import VulkanBackend.Component;
import VulkanEngine.Components.Transform;
import VulkanEngine.BindlessManager.TextureSlot;
import VulkanEngine.MaterialManager;

export namespace App::Components {

enum class RotationMode : std::uint8_t {
    Euler,
    Quaternion
};

class TransformControlComponent : public VulkanEngine::Component {
public:
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    RotationMode rotation_mode = RotationMode::Euler;
    glm::vec3 rotation_euler{0.0f, 0.0f, 0.0f};
    glm::quat rotation_quat{1.0f, 0.0f, 0.0f, 0.0f};
    int texture_slot = 0;

    std::optional<VulkanEngine::MaterialManager::MaterialId> material_id{};
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    void Update(float /*delta_time*/) override {
        auto* transform = GetOwner() != nullptr ? GetOwner()->GetComponent<VulkanEngine::Components::Transform>() : nullptr;
        if (transform) {
            transform->position = position;
            if (rotation_mode == RotationMode::Euler) {
                transform->rotation = glm::quat{glm::radians(rotation_euler)};
            } else {
                transform->rotation = rotation_quat;
            }
        }

        if (material_id.has_value()) {
            auto& mat_mgr = VulkanEngine::MaterialManager::MaterialManager::Get();
            auto& def = mat_mgr.GetMaterial(material_id.value());
            auto current_slot = static_cast<int>(def.texture_slot.value);
            if (current_slot != texture_slot) {
                mat_mgr.UpdateMaterialTextureSlot(
                    material_id.value(),
                    VulkanEngine::BindlessManager::TextureSlot{static_cast<std::uint16_t>(texture_slot)});
            }
        }
    }

    static auto GetFields() {
        return VulkanEngine::make_fields(
            VulkanEngine::field<glm::vec3>("position"),
            VulkanEngine::field<std::uint8_t>("rotation_mode"),
            VulkanEngine::field<glm::vec3>("rotation_euler"),
            VulkanEngine::field<glm::quat>("rotation_quat"),
            VulkanEngine::field<int>("texture_slot")
        );
    }
};

}
