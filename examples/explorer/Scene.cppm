module;

#include <glm/glm.hpp>

// Scene catalog types for the shared explorer.
//
// A scene is data, not a class hierarchy: a definition names itself, declares
// how to build and tear down its entities, optionally exposes an options panel
// and any number of camera paths, and that is the whole contract. The explorer
// owns the engine lifecycle and the camera; a scene only builds content.
export module Examples.Explorer.Scene;

import std;

import VulkanEngine.GameEngine;

import Examples.Explorer.Math;
import Examples.Explorer.Options;

export namespace Examples::Explorer {

// What a scene's load callback is handed. Everything is borrowed and only valid
// during the callback (and, for the engine/registry pointers, for as long as the
// application runs), so a scene must not cache it.
struct SceneContext {
    VulkanEngine::Application::ApplicationContext* app = nullptr;
    VulkanEngine::GameEngine* engine = nullptr;
    std::filesystem::path asset_dir{};
    // Set by the explorer. A scene calls RequestReload() when an option changed
    // something that cannot be updated in place (a different asset, a different
    // grid size, ...); the explorer rebuilds it at the next frame boundary.
    std::function<void()> request_reload{};
    // One-shot host request for this load (the example uses it for
    // `--download <id>`). Empty when there is nothing to do.
    std::string initial_action{};

    [[nodiscard]] VulkanEngine::ComponentRegistry& Registry() const;

    // Creates a Transform + MeshReference entity at `position` and returns its
    // id. Entities created this way must be reported in SceneBuildResult so the
    // explorer can destroy them when switching away.
    [[nodiscard]] VulkanEngine::EntityId CreateMeshEntity(std::uint32_t mesh_id,
                                                          const glm::vec3& position =
                                                              glm::vec3{0.0f}) const;

    void RequestReload() const {
        if (request_reload) request_reload();
    }
};

struct SceneBuildResult {
    std::vector<VulkanEngine::EntityId> entities{};
    Bounds bounds{};
    std::string error{};
    // Informational text shown dim (not an error), e.g. "downloading...".
    std::string notice{};
    bool ok = true;
};

struct SceneDefinition {
    std::string id{};
    std::string name{};
    std::string summary{};
    // SPDX identifier, or "procedural" for generated content. Shown verbatim in
    // the scene list so a catalog of downloaded assets keeps its licensing
    // visible where the user chooses what to load.
    std::string license{};
    std::string attribution{};
    std::string source{};

    std::function<SceneBuildResult(SceneContext&)> load{};
    // Optional teardown for state that is not an entity (cached GPU handles,
    // subscriptions, ...). Entities returned by load are destroyed by the
    // explorer before this runs.
    std::function<void(SceneContext&)> unload{};
    // Per-frame scene behavior: animation, material tweaks, lighting uploads.
    std::function<void(SceneContext&, float)> update{};
    // Declared once after load; the explorer draws it in the options panel.
    std::function<void(OptionsUi&)> options{};

    std::vector<CameraPath> paths{};
};

// What a catalog builder is handed: where the executable lives (to resolve
// bundled assets) and which glTF files the user pointed the explorer at.
struct CatalogContext {
    std::filesystem::path exe_dir{};
    std::filesystem::path asset_dir{};
    std::vector<std::filesystem::path> asset_files{};
};

using SceneCatalog = std::vector<SceneDefinition>;

// World-space bounds of imported geometry, including each submesh's baked node
// placement. Used to frame the camera and to center a spin pivot.
[[nodiscard]] Bounds ComputeBounds(const VulkanEngine::SceneLoader::LoadedMeshData& mesh);

} // namespace Examples::Explorer
