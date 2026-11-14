module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

module Examples.Explorer.Scene;

import std;

import VulkanEngine.GameEngine;
import VulkanEngine.ECS.ComponentRegistry;

import Examples.Explorer.Math;
import Examples.Explorer.Options;

namespace Examples::Explorer {

VulkanEngine::ComponentRegistry& SceneContext::Registry() const {
    return engine->GetContext().component_registry;
}

VulkanEngine::EntityId SceneContext::CreateMeshEntity(std::uint32_t mesh_id,
                                                      const glm::vec3& position) const {
    auto& registry = Registry();
    auto& entity = registry.CreateEntity();
    auto& transform = registry.AddComponent<VulkanEngine::Components::Transform>(entity);
    transform.position = position;
    auto& mesh = registry.AddComponent<VulkanEngine::Components::MeshReference>(entity);
    mesh.loaded_mesh_id = mesh_id;
    return entity.GetId();
}

Bounds ComputeBounds(const VulkanEngine::SceneLoader::LoadedMeshData& mesh) {
    Bounds bounds{};
    if (mesh.positions.empty()) return bounds;

    auto expand = [&bounds](const glm::vec3& point) {
        if (!bounds.valid) {
            bounds.min = point;
            bounds.max = point;
            bounds.valid = true;
            return;
        }
        bounds.min = glm::min(bounds.min, point);
        bounds.max = glm::max(bounds.max, point);
    };

    const auto vertex_count = mesh.positions.size() / 3;
    auto vertex_at = [&mesh](std::uint32_t index) {
        const std::size_t base = static_cast<std::size_t>(index) * 3;
        return glm::vec3{mesh.positions[base + 0], mesh.positions[base + 1],
                         mesh.positions[base + 2]};
    };

    if (mesh.submeshes.empty()) {
        for (std::uint32_t i = 0; i < vertex_count; ++i) expand(vertex_at(i));
        return bounds;
    }

    for (const auto& submesh : mesh.submeshes) {
        // The glTF importer keeps node placement on the submesh (identity for
        // OBJ/bin), so bounds have to apply it rather than assume baked vertices.
        glm::mat4 placement{1.0f};
        const auto& trs = submesh.transform;
        const glm::quat rotation{trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]};
        placement = glm::translate(placement, glm::vec3{trs.position.x, trs.position.y, trs.position.z});
        placement *= glm::mat4_cast(rotation);
        placement = glm::scale(placement, glm::vec3{trs.scale.x, trs.scale.y, trs.scale.z});

        const std::uint32_t end = submesh.index_start + submesh.index_count;
        bool expanded = false;
        for (std::uint32_t i = submesh.index_start; i < end && i < mesh.indices.size(); ++i) {
            const std::uint32_t index = mesh.indices[i];
            if (index >= vertex_count) continue;
            expand(glm::vec3{placement * glm::vec4(vertex_at(index), 1.0f)});
            expanded = true;
        }

        // A submesh without index data still has geometry worth framing.
        if (!expanded) {
            for (std::uint32_t i = 0; i < vertex_count; ++i) expand(vertex_at(i));
            break;
        }
    }

    if (!bounds.valid) {
        for (std::uint32_t i = 0; i < vertex_count; ++i) expand(vertex_at(i));
    }
    return bounds;
}

} // namespace Examples::Explorer
