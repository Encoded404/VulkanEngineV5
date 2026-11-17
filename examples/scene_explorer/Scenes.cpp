module;

#include <cctype>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "scene_explorer_catalog.hpp"

#include <logging/logging_macros.hpp>

module Examples.SceneExplorer.Scenes;

import std;

import logiface;

import VulkanEngine.GameEngine;
import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.TechniqueManager;
import VulkanEngine.Components.MaterialOverride;
import VulkanEngine.MaterialManager;
import VulkanEngine.FileLoaders.Mesh.GltfMaterialImport;
import VulkanEngine.DefaultTextureFactory;
import VulkanEngine.TextureTypes;
import VulkanShared.Storage;

import Examples.Explorer.Download;
import Examples.Explorer.Math;
import Examples.Explorer.Options;
import Examples.Explorer.Scene;

namespace Examples::SceneExplorer {

namespace {

using Examples::Explorer::Bounds;
using Examples::Explorer::CameraPath;
using Examples::Explorer::OptionsUi;
using Examples::Explorer::SceneBuildResult;
using Examples::Explorer::SceneContext;
using Examples::Explorer::DownloadOutcome;
using Examples::Explorer::DownloadState;
using Examples::Explorer::DownloadableAsset;
using Examples::Explorer::SceneDefinition;
using Examples::Explorer::SceneDownloader;
using VulkanEngine::EntityId;
using VulkanEngine::MaterialManager::MaterialDesc;
using VulkanEngine::MaterialManager::MaterialHandle;
using VulkanEngine::MaterialManager::MaterialId;
using VulkanEngine::TechniqueManager::DefaultMeshPerMaterialData;
using VulkanEngine::TechniqueManager::DefaultMeshTechnique;

constexpr std::uint32_t kInvalidMesh = 0xFFFFFFFFu;

// ── shared helpers ──────────────────────────────────────────────────────

[[nodiscard]] std::string LowerText(std::string_view text) {
    std::string lowered{text};
    std::ranges::transform(lowered, lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

[[nodiscard]] std::string LowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

[[nodiscard]] std::vector<std::filesystem::path> DiscoverAssets(
    const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> found{};
    std::error_code ec{};
    if (!std::filesystem::is_directory(directory, ec)) return found;

    // directory_iterator's non-member comparisons are not exported by the std
    // module under libc++, so walk it against the member sentinel instead.
    for (auto it = std::filesystem::directory_iterator(directory, ec);
         it != std::default_sentinel; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = LowerExtension(it->path());
        if (ext == ".glb" || ext == ".gltf") found.push_back(it->path());
    }
    std::ranges::sort(found);
    return found;
}

// Bindless slots for generated textures (a neutral white and per-color 1x1
// swatches), cached for the lifetime of the catalog. Materials without a
// base-color texture would otherwise sample the engine's checkerboard fallback.
struct TextureCache {
    std::uint32_t white = 0;
    std::unordered_map<std::uint64_t, std::uint32_t> swatches{};
    std::vector<std::shared_ptr<VulkanEngine::TextureResource>> owned{};

    [[nodiscard]] std::uint32_t White(SceneContext& ctx) {
        if (white != 0) return white;
        auto texture =
            VulkanEngine::DefaultTextureFactory::CreateWhiteTexture(ctx.engine->GetResourceManager());
        white = ctx.engine->UploadTextureToBindless(
            *ctx.app, texture.get(), VulkanEngine::Textures::TextureSemantic::BaseColor,
            VulkanEngine::Textures::TextureNormalEncoding::Standard, {});
        owned.push_back(std::move(texture));
        return white;
    }

    [[nodiscard]] std::uint32_t Solid(SceneContext& ctx, const glm::vec3& color) {
        const auto to_byte = [](float value) {
            return static_cast<std::uint8_t>(std::clamp(std::lround(value * 255.0f), 0L, 255L));
        };
        const std::uint8_t r = to_byte(color.r);
        const std::uint8_t g = to_byte(color.g);
        const std::uint8_t b = to_byte(color.b);
        const std::uint64_t key = (static_cast<std::uint64_t>(r) << 16) |
                                  (static_cast<std::uint64_t>(g) << 8) |
                                  static_cast<std::uint64_t>(b);
        if (const auto it = swatches.find(key); it != swatches.end()) return it->second;

        auto texture = VulkanEngine::DefaultTextureFactory::CreateSolidColorTexture(
            ctx.engine->GetResourceManager(), std::array<std::uint8_t, 4>{r, g, b, 255});
        const std::uint32_t slot = ctx.engine->UploadTextureToBindless(
            *ctx.app, texture.get(), VulkanEngine::Textures::TextureSemantic::BaseColor,
            VulkanEngine::Textures::TextureNormalEncoding::Standard, {});
        swatches.emplace(key, slot);
        owned.push_back(std::move(texture));
        return slot;
    }
};

[[nodiscard]] glm::vec3 HsvToRgb(float hue, float saturation, float value) {
    const float h = (hue - std::floor(hue)) * 6.0f;
    const float c = value * saturation;
    const float x = c * (1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f));
    const float m = value - c;
    glm::vec3 rgb{0.0f};
    switch (static_cast<int>(h)) {
        case 0: rgb = {c, x, 0.0f}; break;
        case 1: rgb = {x, c, 0.0f}; break;
        case 2: rgb = {0.0f, c, x}; break;
        case 3: rgb = {0.0f, x, c}; break;
        case 4: rgb = {x, 0.0f, c}; break;
        default: rgb = {c, 0.0f, x}; break;
    }
    return rgb + glm::vec3{m};
}


// ── on-demand download catalog ──────────────────────────────────────────

[[nodiscard]] std::string FormatByteSize(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024ull) {
        return std::format("{:.1f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    if (bytes >= 1024ull) {
        return std::format("{:.1f} KiB", static_cast<double>(bytes) / 1024.0);
    }
    return std::format("{} B", bytes);
}

// ── glTF asset scene ────────────────────────────────────────────────────

struct GltfAsset {
    std::uint32_t mesh_id = kInvalidMesh;
    std::vector<MaterialId> materials{};
    std::vector<MaterialHandle<DefaultMeshTechnique>> handles{};
    std::vector<DefaultMeshPerMaterialData> base_data{};
    // Per-material render state, aligned with `materials`, so the unlit view
    // can register a matching unlit material for every entry.
    std::vector<VulkanEngine::MaterialManager::MaterialDesc> descs{};
    std::vector<MaterialHandle<VulkanEngine::TechniqueManager::UnlitTextureTechnique>> unlit_handles{};
    std::vector<MaterialId> unlit_materials{};
    // Per-submesh material ids from the built mesh; the unlit override maps each
    // one to the matching unlit material.
    std::vector<MaterialId> submesh_materials{};
    // Bindless slots this asset owns; released on unload so switching assets
    // cannot accumulate GPU images (and their host-side decoded data).
    std::vector<std::uint32_t> texture_slots{};
    Bounds bounds{};
};

struct AssetSession {
    std::filesystem::path asset_dir{};
    std::vector<std::filesystem::path> files{};
    int index = 0;
    int loaded_index = -1;

    bool spin = false;
    float spin_speed = 24.0f;
    float scale = 1.0f;
    float spin_angle = 0.0f;

       bool unlit = false;
    bool unlit_applied = false;

    bool override_factors = false;
    float roughness = 0.6f;
    float metallic = 0.0f;
    float emissive_strength = 1.0f;
    bool override_applied = false;
    float applied_roughness = 0.6f;
    float applied_metallic = 0.0f;
    float applied_emissive = 1.0f;

    bool reload_requested = false;
    bool rescan_requested = false;

    std::unordered_map<std::string, GltfAsset> cache{};
    TextureCache textures{};

    // On-demand downloads: created lazily on the first load, because the cache
    // root only exists once the engine's per-user storage is up. One session is
    // shared by every scene that shows downloaded assets, so the process has
    // exactly one transfer (two downloaders over one cache root would truncate
    // each other's ".part" file) and one --download one-shot flag.
    std::unique_ptr<SceneDownloader> downloader{};
    std::vector<DownloadableAsset> library{};
    int library_index = 0;
    std::string download_message{};
    std::filesystem::path download_root{};
    bool auto_download_started = false;

    GltfAsset* active_asset = nullptr;
    std::filesystem::path active_path{};
    Bounds active_bounds{};
    EntityId root_entity{};
};

// Presentation state of the library scene: what the user is looking at and
// what is actually loaded. It is separate from the session because a scene
// switch must not lose the filter, and because the session is shared with
// `sample-assets`, which has no library selection at all.
struct LibraryState {
    std::string search{};
    int license_filter = 0;   // 0 = all, otherwise index into the sorted license list
    int category_filter = 0;  // 0 = all, otherwise index into the sorted category list
    int selected = 0;         // index into the unfiltered catalog
    // Catalog index the loaded scene is showing, or -1 when nothing is open.
    // The library's unload resets it, and its load sets it as an output.
    int open_index = -1;
};

void EnsureDownloader(SceneContext& ctx, AssetSession& session) {
    if (session.downloader) return;
    if (ctx.app == nullptr || ctx.app->storage == nullptr) {
        session.download_message =
            "Per-user storage is unavailable, so on-demand downloads are disabled.";
        return;
    }
    session.download_root = ctx.app->storage->CacheDir() / "scenes";
    session.library = BuildDownloadCatalog();
    session.downloader = std::make_unique<SceneDownloader>(session.download_root);
    session.downloader->SetCatalog(session.library);
}

// Local assets plus everything already downloaded. Sorted and deduplicated, so
// the combo order is stable across frames.
[[nodiscard]] std::vector<std::filesystem::path> CollectAssetFiles(AssetSession& session) {
    std::vector<std::filesystem::path> files = DiscoverAssets(session.asset_dir);
    if (session.downloader) {
        for (const auto& downloaded : session.downloader->InstalledFiles()) {
            files.push_back(downloaded);
        }
    }
    std::ranges::sort(files);
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

[[nodiscard]] std::string AssetLabel(const std::filesystem::path& path) {
    const std::string parent = path.parent_path().filename().string();
    if (parent.empty()) return path.filename().string();
    return parent + "/" + path.filename().string();
}

// Applies the scene's scale and spin about the model's own center. The entity
// transform is R*S*v + p, so the pivot stays fixed by setting
// p = center - R*(S*center).
void ApplyModelTransform(SceneContext& ctx, const AssetSession& session) {
    // Nothing loaded (or the load failed) means there is no root entity to
    // move; a default EntityId has generation 0 and must not be looked up.
    if (!session.root_entity.IsValid()) return;
    auto& registry = ctx.Registry();
    auto* entity = registry.TryGetEntity(session.root_entity);
    if (entity == nullptr) return;
    auto* transform = entity->GetComponent<VulkanEngine::Components::Transform>();
    if (transform == nullptr) return;

    const glm::vec3 center = session.active_bounds.Center() * session.scale;
    const glm::quat rotation =
        glm::angleAxis(glm::radians(session.spin_angle), glm::vec3{0.0f, 1.0f, 0.0f});
    transform->scale = glm::vec3{session.scale};
    transform->rotation = rotation;
    transform->position = center - rotation * center;
}

[[nodiscard]] Bounds ScaledBounds(const Bounds& bounds, float scale) {
    const float radius = bounds.Radius() * scale;
    return Bounds{glm::vec3{-radius}, glm::vec3{radius}, true};
}

[[nodiscard]] bool LoadGltfAsset(SceneContext& ctx, const std::filesystem::path& path,
                                 TextureCache& textures, GltfAsset& asset, std::string& error) {
    using namespace VulkanEngine;

    FileLoaders::Mesh::GltfImportResult import{};
    try {
        import = FileLoaders::Mesh::ImportGltfMaterials(path);
    } catch (const std::exception& err) {
        error = std::string{"Could not read '"} + path.filename().string() + "': " + err.what();
        return false;
    }

    std::unordered_map<std::uint64_t, std::uint32_t> uploaded{};
    std::size_t fallback_textures = 0;
    auto sampler_for = [&import](const FileLoaders::Mesh::GltfTextureSlot& slot) {
        if (slot.texture_index >= import.textures.size()) return Textures::SamplerDesc{};
        const auto& ref = import.textures[slot.texture_index];
        if (ref.sampler_index == FileLoaders::Mesh::kNoGltfSampler ||
            ref.sampler_index >= import.samplers.size()) {
            return Textures::SamplerDesc{};
        }
        return FileLoaders::Mesh::ToSamplerDesc(import.samplers[ref.sampler_index]);
    };

    auto resolve = [&](const FileLoaders::Mesh::GltfTextureSlot& slot,
                       Textures::TextureSemantic semantic,
                       Textures::TextureNormalEncoding encoding) -> std::uint32_t {
        if (!slot.present || slot.texture_index >= import.textures.size()) {
            return textures.White(ctx);
        }
        const auto& ref = import.textures[slot.texture_index];
        if (!ref.valid || ref.image_index >= import.images.size()) {
            return textures.White(ctx);
        }
        const std::uint64_t key = (static_cast<std::uint64_t>(ref.image_index) << 32) |
                                  (static_cast<std::uint64_t>(ref.sampler_index) << 8) |
                                  static_cast<std::uint64_t>(semantic);
        if (const auto it = uploaded.find(key); it != uploaded.end()) return it->second;

        std::uint32_t slot_id = ctx.engine->LoadTextureFromGltfImage(
            *ctx.app, import.images[ref.image_index], semantic, encoding, sampler_for(slot),
            path.parent_path());
        if (slot_id == 0) {
            // Decode/upload failed (or the bindless pool is full): a white
            // swatch keeps the material visible instead of the checkerboard.
            slot_id = textures.White(ctx);
            ++fallback_textures;
        }
        uploaded.emplace(key, slot_id);
        asset.texture_slots.push_back(slot_id);
        return slot_id;
    };

    asset.materials.reserve(import.materials.size());
    asset.handles.reserve(import.materials.size());
    asset.base_data.reserve(import.materials.size());

    for (const auto& material : import.materials) {
        DefaultMeshPerMaterialData data = TechniqueManager::FromGltfMaterial(material);

        if (material.base_color_texture.present) {
            data.albedo_texture = resolve(material.base_color_texture,
                                          Textures::TextureSemantic::BaseColor,
                                          Textures::TextureNormalEncoding::Standard);
        } else {
            data.albedo_texture = textures.Solid(
                ctx, glm::vec3{material.base_color_factor[0], material.base_color_factor[1],
                               material.base_color_factor[2]});
        }
        // Bindless slot 0 means "no map" to the shader, which then falls back to
        // the geometric normal / material factors. Only the base-color slot is
        // always sampled, so only that one needs a generated swatch.
        const auto resolve_optional = [&](const FileLoaders::Mesh::GltfTextureSlot& slot,
                                          Textures::TextureSemantic semantic,
                                          Textures::TextureNormalEncoding encoding)
            -> std::uint32_t {
            if (!slot.present || slot.texture_index >= import.textures.size()) return 0;
            const auto& ref = import.textures[slot.texture_index];
            if (!ref.valid || ref.image_index >= import.images.size()) return 0;
            return resolve(slot, semantic, encoding);
        };

        data.normal_texture = resolve_optional(material.normal_texture,
                                               Textures::TextureSemantic::Normal,
                                               material.normal_encoding);
        // glTF allows occlusion and metallic-roughness to be separate textures;
        // the engine packs both into one ORM slot, so either supplies it.
        const auto& orm_slot = material.metallic_roughness_texture.present
                                   ? material.metallic_roughness_texture
                                   : material.occlusion_texture;
        data.orm_texture = resolve_optional(orm_slot, Textures::TextureSemantic::ORM,
                                            Textures::TextureNormalEncoding::Standard);
        data.emissive_texture = resolve_optional(material.emissive_texture,
                                                 Textures::TextureSemantic::Emissive,
                                                 Textures::TextureNormalEncoding::Standard);

        MaterialDesc desc{};
        switch (material.alpha_mode) {
            case FileLoaders::Mesh::GltfAlphaMode::Blend:
                desc.blend = MaterialManager::BlendMode::Transparent;
                desc.depth_write = false;
                break;
            case FileLoaders::Mesh::GltfAlphaMode::Mask:
                desc.blend = MaterialManager::BlendMode::Cutout;
                break;
            case FileLoaders::Mesh::GltfAlphaMode::Opaque:
            default:
                desc.blend = MaterialManager::BlendMode::Opaque;
                break;
        }
        desc.double_sided = material.double_sided;

        auto handle = ctx.engine->GetContext().GetMaterialManager().Register<DefaultMeshTechnique>(
            desc, data);
        asset.descs.push_back(desc);
        asset.materials.push_back(handle.Valid() ? MaterialId{handle.Id()} : MaterialId{0});
        asset.handles.push_back(handle);
        asset.base_data.push_back(data);
    }

    try {
        const std::vector<MaterialId>* bindings =
            asset.materials.empty() ? nullptr : &asset.materials;
        auto loaded = SceneLoader::LoadMeshFromFilePath(path, bindings);
        asset.bounds = Examples::Explorer::ComputeBounds(loaded);
        asset.submesh_materials.reserve(loaded.submeshes.size());
        for (const auto& submesh : loaded.submeshes) {
            asset.submesh_materials.push_back(submesh.material_id);
        }
        asset.mesh_id = ctx.engine->GetMeshRegistry().Register(SceneLoader::ToMeshData(loaded));
    } catch (const std::exception& err) {
        error = std::string{"Could not build mesh for '"} + path.filename().string() +
                "': " + err.what();
        return false;
    }

    LOGIFACE_LOG(info, "SceneExplorer: loaded '" + path.filename().string() + "' -> mesh " +
                           std::to_string(asset.mesh_id) + ", " +
                           std::to_string(asset.materials.size()) + " materials, " +
                           std::to_string(import.images.size()) + " images, " +
                           std::to_string(import.textures.size()) + " textures" +
                           (fallback_textures > 0
                                ? " (" + std::to_string(fallback_textures) + " fell back to white)"
                                : ""));
    return true;
}

// Lazily registers one unlit material per lit material, reusing the exact same
// albedo bindless slot (an unlit override must not upload or free textures).
void EnsureUnlitMaterials(SceneContext& ctx, GltfAsset& asset) {
    if (asset.unlit_handles.size() == asset.materials.size()) return;
    auto& material_manager = ctx.engine->GetContext().GetMaterialManager();
    asset.unlit_handles.clear();
    asset.unlit_materials.clear();
    asset.unlit_handles.reserve(asset.materials.size());
    for (std::size_t i = 0; i < asset.materials.size(); ++i) {
        VulkanEngine::TechniqueManager::UnlitPerMaterialData data{};
        if (i < asset.base_data.size()) {
            data.albedo_texture = asset.base_data[i].albedo_texture;
            data.alpha_cutoff = asset.base_data[i].alpha_cutoff;
        }
        const VulkanEngine::MaterialManager::MaterialDesc desc =
            i < asset.descs.size() ? asset.descs[i]
                                   : VulkanEngine::MaterialManager::MaterialDesc{};
        auto handle = material_manager.Register<VulkanEngine::TechniqueManager::UnlitTextureTechnique>(desc, data);
        asset.unlit_handles.push_back(handle);
        asset.unlit_materials.push_back(handle.Valid() ? MaterialId{handle.Id()} : MaterialId{0});
    }
}

// Maps every submesh to the unlit sibling of its lit material, or back to the
// asset's own material when disabled. The override is installed once at load;
// toggling only rewrites its sparse ref list.
void ApplyUnlit(SceneContext& ctx, AssetSession& state, bool enabled) {
    if (state.active_asset == nullptr) return;
    auto* entity = ctx.Registry().TryGetEntity(state.root_entity);
    if (entity == nullptr) return;
    auto* override_comp =
        ctx.Registry().GetComponent<VulkanEngine::Components::MaterialOverride>(*entity);
    if (override_comp == nullptr) return;

    override_comp->reported_signatures.clear();
    if (!enabled) {
        override_comp->submeshes.clear();
        return;
    }

    EnsureUnlitMaterials(ctx, *state.active_asset);
    GltfAsset& asset = *state.active_asset;
    override_comp->submeshes.assign(asset.submesh_materials.size(),
                                    VulkanEngine::MaterialManager::MaterialRef::Unset());
    for (std::size_t i = 0; i < asset.submesh_materials.size(); ++i) {
        const auto found = std::ranges::find(asset.materials, asset.submesh_materials[i]);
        if (found == asset.materials.end()) continue;
        const auto index = static_cast<std::size_t>(
            std::distance(asset.materials.begin(), found));
        if (index < asset.unlit_handles.size() && asset.unlit_handles[index].Valid()) {
            override_comp->submeshes[i] = asset.unlit_handles[index].Ref();
        }
    }
}

// Returns every GPU resource an imported asset owns. Without this, switching
// assets would keep the previous asset's materials, meshes and bindless texture
// slots alive; on a memory-constrained device the next asset then fails to bind
// its images.
void ReleaseGltfAsset(SceneContext& ctx, GltfAsset& asset) {
    auto& engine = *ctx.engine;

    auto& material_manager = engine.GetContext().GetMaterialManager();
    for (auto& handle : asset.handles) {
        if (handle.Valid()) material_manager.Destroy(MaterialId{handle.Id()});
    }
    for (auto& handle : asset.unlit_handles) {
        if (handle.Valid()) material_manager.Destroy(MaterialId{handle.Id()});
    }
    asset.unlit_handles.clear();
    asset.unlit_materials.clear();
    asset.handles.clear();
    asset.materials.clear();
    asset.base_data.clear();
    asset.descs.clear();
    asset.submesh_materials.clear();

    auto& bindless = engine.GetBindlessManager();
    const std::uint32_t frame = ctx.app != nullptr ? ctx.app->frame.frame_counter : 0;
    std::ranges::sort(asset.texture_slots);
    asset.texture_slots.erase(std::unique(asset.texture_slots.begin(), asset.texture_slots.end()),
                              asset.texture_slots.end());
    for (const std::uint32_t slot : asset.texture_slots) {
        if (slot == 0) continue;
        if (const auto handle = bindless.GetHandle(slot); handle.has_value()) {
            bindless.ReleaseSlot(*handle, frame);
        }
    }
    asset.texture_slots.clear();

    if (asset.mesh_id != kInvalidMesh) {
        engine.GetMeshRegistry().Release(asset.mesh_id, engine.GetMeshManager());
        asset.mesh_id = kInvalidMesh;
    }
    asset.bounds = {};
}

// Imports one asset path and makes it the scene's content. The per-path import
// cache lives here: a hit skips the import, a miss inserts the result. The
// caller owns the index bookkeeping, because only it knows whether it has a
// file list (`sample-assets`) or a catalog selection (the library).
[[nodiscard]] SceneBuildResult LoadSessionAsset(SceneContext& ctx, AssetSession& session,
                                                const std::filesystem::path& path) {
    SceneBuildResult result{};

    GltfAsset* asset = nullptr;
    if (const auto it = session.cache.find(path.string()); it != session.cache.end()) {
        asset = &it->second;
    } else {
        std::string error{};
        GltfAsset loaded{};
        if (!LoadGltfAsset(ctx, path, session.textures, loaded, error) ||
            loaded.mesh_id == kInvalidMesh) {
            result.ok = false;
            result.error = error.empty() ? "The asset produced no mesh." : error;
            session.active_asset = nullptr;
            session.active_path.clear();
            return result;
        }
        asset = &session.cache.emplace(path.string(), std::move(loaded)).first->second;
    }

    session.active_asset = asset;
    session.active_path = path;
    session.active_bounds = asset->bounds;
    session.spin_angle = 0.0f;
    session.override_applied = false;
    session.root_entity = ctx.CreateMeshEntity(asset->mesh_id);
    if (auto* root = ctx.Registry().TryGetEntity(session.root_entity)) {
        auto& override_comp =
            ctx.Registry().AddComponent<VulkanEngine::Components::MaterialOverride>(*root);
        override_comp.mesh_id = asset->mesh_id;
    }
    session.unlit_applied = false;
    result.entities.push_back(session.root_entity);
    result.bounds = ScaledBounds(asset->bounds, session.scale);
    ApplyModelTransform(ctx, session);
    return result;
}

// Consumes the downloader's one-shot completion record, refreshes the file
// list and queues a reload on success. The caller still owns the selection
// policy: `sample-assets` maps the path back into its file list, the library
// maps the asset id back into the catalog.
[[nodiscard]] std::optional<DownloadOutcome> PollDownload(SceneContext& ctx,
                                                          AssetSession& session) {
    if (session.downloader == nullptr) return std::nullopt;
    auto outcome = session.downloader->TakeOutcome();
    if (!outcome.has_value()) return std::nullopt;

    if (outcome->ok) {
        session.download_message = "Downloaded " + outcome->path.filename().string();
        LOGIFACE_LOG(info, "SceneExplorer: downloaded '" + outcome->path.string() + "' (" +
                               FormatByteSize(std::filesystem::file_size(outcome->path)) + ")");
        session.files = CollectAssetFiles(session);
        ctx.RequestReload();
    } else {
        session.download_message = "Download failed: " + outcome->error;
        LOGIFACE_LOG(warn, "SceneExplorer: " + session.download_message);
    }
    return outcome;
}

// Consumes the reload and rescan requests a scene set from its options panel.
// Returns true when a reload is already queued, so the caller must stop this
// frame. The reload tears the scene down first, and the unload releases and
// drops every cached import, so the next load re-reads the file from disk.
[[nodiscard]] bool ConsumeSessionReload(SceneContext& ctx, AssetSession& session) {
    if (session.rescan_requested) {
        session.rescan_requested = false;
        session.files = CollectAssetFiles(session);
        session.index = session.files.empty()
                            ? 0
                            : std::clamp(session.index, 0,
                                         static_cast<int>(session.files.size()) - 1);
        ctx.RequestReload();
        return true;
    }
    if (session.reload_requested) {
        session.reload_requested = false;
        ctx.RequestReload();
        return true;
    }
    return false;
}

// Per-frame animation and material state: spin, scale, unlit and factor
// overrides. Selection and index changes stay with the scene, because they
// differ between the file-list scene and the catalog scene.
void UpdateSession(SceneContext& ctx, AssetSession& session, float delta_time) {
    if (session.active_asset == nullptr) return;
    if (session.unlit != session.unlit_applied) {
        ApplyUnlit(ctx, session, session.unlit);
        session.unlit_applied = session.unlit;
    }

    if (session.spin) {
        session.spin_angle =
            std::fmod(session.spin_angle + session.spin_speed * delta_time, 360.0f);
    }
    ApplyModelTransform(ctx, session);

    const auto apply = [&](const std::function<void(DefaultMeshPerMaterialData&)>& mutate) {
        for (auto& handle : session.active_asset->handles) {
            if (!handle.Valid()) continue;
            handle.Modify<DefaultMeshPerMaterialData>(mutate);
        }
    };

    if (session.override_factors) {
        const bool changed = !session.override_applied ||
                             session.applied_roughness != session.roughness ||
                             session.applied_metallic != session.metallic ||
                             session.applied_emissive != session.emissive_strength;
        if (changed) {
            apply([&session](DefaultMeshPerMaterialData& data) {
                data.roughness_factor = session.roughness;
                data.metallic_factor = session.metallic;
                data.emissive_factor[3] = session.emissive_strength;
            });
            session.applied_roughness = session.roughness;
            session.applied_metallic = session.metallic;
            session.applied_emissive = session.emissive_strength;
            session.override_applied = true;
        }
    } else if (session.override_applied) {
        // Restore what the asset declared.
        const auto& base = session.active_asset->base_data;
        for (std::size_t i = 0; i < session.active_asset->handles.size() && i < base.size(); ++i) {
            auto& handle = session.active_asset->handles[i];
            if (!handle.Valid()) continue;
            const DefaultMeshPerMaterialData original = base[i];
            handle.Modify<DefaultMeshPerMaterialData>(
                [original](DefaultMeshPerMaterialData& data) { data = original; });
        }
        session.override_applied = false;
    }
}

// Releases every cached import. A scene unload calls this, so switching scenes
// or reloading re-imports from disk instead of keeping GPU memory alive.
void UnloadSession(SceneContext& ctx, AssetSession& session) {
    session.active_asset = nullptr;
    session.active_path.clear();
    for (auto& [path, asset] : session.cache) {
        (void)path;
        ReleaseGltfAsset(ctx, asset);
    }
    session.cache.clear();
    session.loaded_index = -1;
}

// The Transform, Shading and Material override sections are safe to declare
// once per load: none of their values depends on per-frame state.
void BuildSessionOptions(OptionsUi& ui, AssetSession& session) {
    ui.Section("Transform");
    ui.Checkbox("Spin", session.spin);
    ui.SliderFloat("Spin speed", session.spin_speed, -180.0f, 180.0f, "%.0f deg/s");
    ui.SliderFloat("Scale", session.scale, 0.01f, 20.0f, "%.2f");

    ui.Section("Shading");
    ui.Checkbox("Unlit", session.unlit,
                "Sample the albedo texture only: no lights, normal map or ORM.");

    ui.Section("Material override");
    ui.Checkbox("Override factors", session.override_factors,
                "Applies one set of factors to every lit material of this asset.");
    if (session.override_factors) {
        ui.SliderFloat("Roughness", session.roughness, 0.0f, 1.0f, "%.2f");
        ui.SliderFloat("Metallic", session.metallic, 0.0f, 1.0f, "%.2f");
        ui.SliderFloat("Emissive strength", session.emissive_strength, 0.0f, 10.0f, "%.2f");
    }
}

// The combo-box download panel of `sample-assets`. It owns a Dynamic callback
// because its contents depend on the live install and transfer state.
void BuildDownloadSection(OptionsUi& ui, AssetSession& session) {
    ui.Section("Download");
    ui.Dynamic([&session](OptionsUi& download) {
        if (session.downloader == nullptr) {
            download.TextWrapped(session.download_message.empty()
                                     ? "On-demand downloads are unavailable."
                                     : session.download_message);
            return;
        }

        SceneDownloader& downloader = *session.downloader;
        const auto& catalog = downloader.Catalog();
        if (catalog.empty()) {
            download.TextDisabled("This build has no downloadable assets.");
            return;
        }

        std::vector<std::string> labels{};
        labels.reserve(catalog.size());
        for (const auto& asset : catalog) labels.push_back(asset.name);
        std::vector<std::string_view> views{};
        views.reserve(labels.size());
        for (const auto& label : labels) views.push_back(label);

        session.library_index =
            std::clamp(session.library_index, 0, static_cast<int>(catalog.size()) - 1);
        download.Combo("Asset library", session.library_index, views,
                       "Fetched on demand into the per-user cache; nothing is bundled.");

        const DownloadableAsset& asset =
            catalog[static_cast<std::size_t>(session.library_index)];
        download.TextDisabled(asset.license + "  ·  " + FormatByteSize(asset.size));
        download.TextWrapped(asset.summary);
        download.TextDisabled(asset.attribution);

        const auto progress = downloader.Progress();
        const bool installed = downloader.IsInstalled(asset);
        const bool running_this =
            progress.state == DownloadState::Running && progress.asset_id == asset.id;

        if (installed) {
            download.TextDisabled("Installed in the cache.");
            download.Button("Remove downloaded asset", [&session, asset] {
                if (session.downloader->Remove(asset)) {
                    session.download_message = "Removed " + asset.name + " from the cache.";
                    session.rescan_requested = true;
                }
            });
        } else if (running_this) {
            download.ProgressBar(progress.Fraction(),
                                 FormatByteSize(progress.bytes_received) + " / " +
                                     FormatByteSize(progress.bytes_total));
            download.Button("Cancel download", [&session] { session.downloader->Cancel(); });
        } else {
            download.Button("Download and open", [&session, asset] {
                if (session.downloader->Start(asset)) {
                    session.download_message = "Downloading " + asset.name + "...";
                } else {
                    session.download_message =
                        "Another download is already running (one at a time).";
                }
            });
        }

        if (progress.state == DownloadState::Running && !running_this &&
            !progress.asset_id.empty()) {
            download.TextDisabled("Downloading " + progress.asset_id + "...");
        }
        if (!session.download_message.empty()) {
            download.TextWrapped(session.download_message);
        }
    });
}

// Honors `--download <id>` exactly once per process. Both scenes call it; the
// flag it sets lives in the shared session, so whichever scene loads first wins
// and neither repeats the attempt on reload.
[[nodiscard]] std::optional<std::size_t> StartInitialDownload(SceneContext& ctx,
                                                             AssetSession& session) {
    if (ctx.initial_action.empty() || session.auto_download_started) return std::nullopt;
    session.auto_download_started = true;

    if (session.downloader == nullptr) {
        session.download_message = "Downloads are unavailable.";
        return std::nullopt;
    }

    const auto found = std::ranges::find_if(session.library, [&ctx](const auto& asset) {
        return asset.id == ctx.initial_action;
    });
    if (found == session.library.end()) {
        session.download_message = "Unknown --download id '" + ctx.initial_action + "'.";
        LOGIFACE_LOG(warn, "SceneExplorer: " + session.download_message);
        return std::nullopt;
    }

    const auto index =
        static_cast<std::size_t>(std::distance(session.library.begin(), found));
    if (session.downloader->IsInstalled(*found)) {
        session.download_message = found->name + " is already installed.";
        LOGIFACE_LOG(info, "SceneExplorer: " + session.download_message);
    } else if (session.downloader->Start(*found)) {
        session.download_message = "Downloading " + found->name + "...";
        LOGIFACE_LOG(info, "SceneExplorer: " + session.download_message);
    } else {
        session.download_message = "Could not start the download.";
        LOGIFACE_LOG(warn, "SceneExplorer: " + session.download_message);
    }
    return index;
}

[[nodiscard]] SceneDefinition MakeSampleAssetsScene(std::shared_ptr<AssetSession> session) {
    SceneDefinition scene{};
    scene.id = "sample-assets";
    scene.name = "glTF Sample Assets";
    scene.summary =
        "Browses .glb/.gltf files from an asset directory and switches between them. Point "
        "--assets at a folder of glTF Sample Assets (or any downloaded scene) to populate it. "
        "Material overrides are live.";
    scene.license = "per-asset";
    scene.attribution =
        "Third-party assets keep their own licenses (prefer CC0 or CC BY 4.0). Keep the "
        "license and attribution files with the download.";
    scene.source = "Khronos glTF-Sample-Assets, or any local glTF download.";

    scene.load = [session](SceneContext& ctx) -> SceneBuildResult {
        SceneBuildResult result{};
        EnsureDownloader(ctx, *session);
        session->files = CollectAssetFiles(*session);
        if (const auto requested = StartInitialDownload(ctx, *session); requested.has_value()) {
            session->library_index = static_cast<int>(*requested);
        }

        if (session->files.empty()) {
            const bool downloading =
                session->downloader != nullptr &&
                session->downloader->Progress().state == DownloadState::Running;
            result.ok = downloading;
            if (downloading) {
                result.notice = session->download_message;
            } else {
                result.error =
                    "No .glb/.gltf files found in '" + session->asset_dir.string() +
                    "'. Pass --assets <dir>, download one in the panel, or use --download <id>.";
            }
            return result;
        }
        session->index = std::clamp(session->index, 0,
                                    static_cast<int>(session->files.size()) - 1);
        result = LoadSessionAsset(
            ctx, *session,
            session->files[static_cast<std::size_t>(session->index)]);
        // Remember the attempt whether it worked or not, so the update loop does
        // not re-import (and re-log) a broken file every frame; Reload retries it.
        session->loaded_index = session->index;
        return result;
    };

    scene.unload = [session](SceneContext& ctx) { UnloadSession(ctx, *session); };

    scene.update = [session](SceneContext& ctx, float delta_time) {
        if (auto outcome = PollDownload(ctx, *session);
            outcome.has_value() && outcome->ok) {
            const auto found = std::ranges::find(session->files, outcome->path);
            if (found != session->files.end()) {
                session->index = static_cast<int>(
                    std::distance(session->files.begin(), found));
            }
            session->loaded_index = -1;
            return;
        }
        if (ConsumeSessionReload(ctx, *session)) return;
        if (session->files.empty()) return;
        if (session->index != session->loaded_index) {
            ctx.RequestReload();
            return;
        }
        UpdateSession(ctx, *session, delta_time);
    };

    scene.options = [session](OptionsUi& ui) {
        ui.Section("Asset");
        if (session->files.empty()) {
            ui.TextDisabled("No glTF files discovered.");
            ui.TextWrapped("Pass --assets <dir> at launch, or --asset <file> (repeatable), then "
                           "press Rescan.");
        } else {
            std::vector<std::string> labels{};
            labels.reserve(session->files.size());
            for (const auto& file : session->files) labels.push_back(AssetLabel(file));
            std::vector<std::string_view> views{};
            views.reserve(labels.size());
            for (const auto& label : labels) views.push_back(label);
            ui.Combo("Asset", session->index, views, "Switching rebuilds the scene.");
            ui.TextDisabled(std::to_string(session->index + 1) + " / " +
                            std::to_string(session->files.size()) + " files");
        }
        ui.Button("Rescan directory", [session] { session->rescan_requested = true; });
        ui.SameLine();
        ui.Button("Reload asset", [session] { session->reload_requested = true; });

        BuildSessionOptions(ui, *session);
        BuildDownloadSection(ui, *session);
    };

    return scene;
}

// ── asset library: browse, download, open ───────────────────────────────

// The library's panel. The whole live part sits in one Dynamic callback because
// the explorer declares a scene's options once per load, and this UI depends on
// the current filter, selection and transfer state.
void BuildLibrarySection(OptionsUi& ui, AssetSession& session, LibraryState& state) {
    if (session.downloader == nullptr) {
        ui.TextWrapped(session.download_message.empty() ? "On-demand downloads are unavailable."
                                                        : session.download_message);
        return;
    }
    SceneDownloader& downloader = *session.downloader;
    const auto& catalog = downloader.Catalog();
    if (catalog.empty()) {
        ui.TextDisabled("This build has no downloadable assets.");
        return;
    }

    // The filter values come from the catalog every frame, so a new entry or a
    // new category shows up in the combos with no extra state to keep in sync.
    // An empty value is the "all" slot and sorts first.
    std::vector<std::string> licenses{""};
    std::vector<std::string> categories{""};
    for (const auto& asset : catalog) {
        if (std::ranges::find(licenses, asset.license) == licenses.end()) {
            licenses.push_back(asset.license);
        }
        if (std::ranges::find(categories, asset.category) == categories.end()) {
            categories.push_back(asset.category);
        }
    }
    std::ranges::sort(licenses);
    std::ranges::sort(categories);
    state.license_filter =
        std::clamp(state.license_filter, 0, static_cast<int>(licenses.size()) - 1);
    state.category_filter =
        std::clamp(state.category_filter, 0, static_cast<int>(categories.size()) - 1);

    // Declared before the list so their ImGui ids stay put while the list
    // length changes.
    ui.InputText("Search", state.search, 128,
                 "Matches name, summary, attribution and category.");
    std::vector<std::string> license_labels{};
    license_labels.reserve(licenses.size());
    for (const auto& value : licenses) {
        license_labels.push_back(value.empty() ? "All licenses" : value);
    }
    std::vector<std::string> category_labels{};
    category_labels.reserve(categories.size());
    for (const auto& value : categories) {
        category_labels.push_back(value.empty() ? "All categories" : value);
    }
    std::vector<std::string_view> license_views{};
    license_views.reserve(license_labels.size());
    for (const auto& label : license_labels) license_views.push_back(label);
    std::vector<std::string_view> category_views{};
    category_views.reserve(category_labels.size());
    for (const auto& label : category_labels) category_views.push_back(label);
    ui.Combo("License", state.license_filter, license_views);
    ui.Combo("Category", state.category_filter, category_views);

    const std::vector<std::size_t> visible =
        FilterCatalog(catalog, state.search, licenses[static_cast<std::size_t>(state.license_filter)],
                      categories[static_cast<std::size_t>(state.category_filter)]);
    state.selected = std::clamp(state.selected, 0, static_cast<int>(catalog.size()) - 1);

    ui.TextDisabled(std::to_string(visible.size()) + " of " + std::to_string(catalog.size()) +
                    " entries");
    for (const std::size_t index : visible) {
        const DownloadableAsset& asset = catalog[index];
        std::string label = asset.name + "  ·  " + asset.license + "  ·  " +
                            FormatByteSize(asset.size);
        if (downloader.IsInstalled(asset)) label += "  ·  installed";
        ui.Selectable(label, state.selected == static_cast<int>(index),
                      [&state, index] { state.selected = static_cast<int>(index); },
                      asset.summary);
    }

    const DownloadableAsset& asset = catalog[static_cast<std::size_t>(state.selected)];
    ui.Separator();
    ui.Text(asset.name);
    ui.TextDisabled(asset.category + "  ·  " + asset.license + "  ·  " +
                    FormatByteSize(asset.size));
    ui.TextWrapped(asset.summary);
    ui.TextDisabled(asset.attribution);
    ui.TextWrapped("Source: " + asset.source_page);

    const auto progress = downloader.Progress();
    const bool installed = downloader.IsInstalled(asset);
    const bool running_this =
        progress.state == DownloadState::Running && progress.asset_id == asset.id;
    const int selected = state.selected;

    if (installed) {
        ui.TextDisabled(state.open_index == selected ? "Open in the scene."
                                                     : "Installed in the cache.");
        ui.Button("Open", [&session, &state, selected] {
            state.selected = selected;
            session.reload_requested = true;
        });
        ui.SameLine();
        ui.Button("Remove downloaded asset", [&session, &state, asset, selected] {
            if (!session.downloader->Remove(asset)) {
                session.download_message =
                    "Could not remove " + asset.name + " (it may still be downloading).";
                return;
            }
            session.download_message = "Removed " + asset.name + " from the cache.";
            // Removing a merely selected entry leaves the scene alone; removing
            // the open one has to drop it from view.
            if (state.open_index == selected) session.reload_requested = true;
        });
    } else if (running_this) {
        ui.ProgressBar(progress.Fraction(), FormatByteSize(progress.bytes_received) + " / " +
                                                 FormatByteSize(progress.bytes_total));
        ui.Button("Cancel download", [&session] { session.downloader->Cancel(); });
    } else {
        ui.Button("Download and open", [&session, asset] {
            if (session.downloader->Start(asset)) {
                session.download_message = "Downloading " + asset.name + "...";
            } else {
                session.download_message =
                    "Another download is already running (one at a time).";
            }
        });
    }

    if (progress.state == DownloadState::Running && !running_this &&
        !progress.asset_id.empty()) {
        ui.TextDisabled("Downloading " + progress.asset_id + "...");
    }
    if (!session.download_message.empty()) {
        ui.TextWrapped(session.download_message);
    }
}

[[nodiscard]] SceneDefinition MakeAssetLibraryScene(std::shared_ptr<AssetSession> session,
                                                    std::shared_ptr<LibraryState> state) {
    SceneDefinition scene{};
    scene.id = "asset-library";
    scene.name = "Asset Library";
    scene.summary =
        "Browses the pinned sample-asset catalog with license, size and install state, and "
        "opens a downloaded asset for viewing.";
    scene.license = "per-asset";
    scene.attribution =
        "Each entry keeps its own license and attribution; see the entry details and "
        "credits/ next to the executable.";
    scene.source = "Khronos glTF-Sample-Assets and other pinned permissive sources.";

    scene.load = [session, state](SceneContext& ctx) -> SceneBuildResult {
        SceneBuildResult result{};
        state->open_index = -1;
        EnsureDownloader(ctx, *session);
        if (const auto requested = StartInitialDownload(ctx, *session); requested.has_value()) {
            state->selected = static_cast<int>(*requested);
        }
        if (session->downloader == nullptr) {
            result.notice = session->download_message.empty()
                                ? "On-demand downloads are unavailable."
                                : session->download_message;
            return result;
        }

        const auto& catalog = session->downloader->Catalog();
        if (catalog.empty()) {
            result.notice = "This build has no downloadable assets.";
            return result;
        }
        state->selected = std::clamp(state->selected, 0, static_cast<int>(catalog.size()) - 1);
        const DownloadableAsset& asset = catalog[static_cast<std::size_t>(state->selected)];

        if (!session->downloader->IsInstalled(asset)) {
            // A missing download is not a scene error: nothing is broken, the
            // user just has not asked for this file yet. LoadScene logs
            // ok == false as an error, so report the empty scene as fine.
            result.notice = "Download \"" + asset.name + "\" to view it.";
            if (!session->download_message.empty()) {
                result.notice += "\n" + session->download_message;
            }
            return result;
        }

        result = LoadSessionAsset(ctx, *session, session->downloader->InstallPath(asset));
        if (!result.ok) return result;

        state->open_index = state->selected;

        // One orbit around the loaded asset, so Path mode is useful for every
        // entry without declaring a path per catalog row. The pivot is where
        // ApplyModelTransform keeps the model's own center, not the symmetric
        // box the camera frames: Bounds::Center() of that box is the origin.
        CameraPath orbit{};
        orbit.name = asset.name + " orbit";
        orbit.closed = true;
        const glm::vec3 pivot = session->active_bounds.Center() * session->scale;
        const float radius = result.bounds.Radius();
        // A PathKey target of exactly (0,0,0) is the "unset, aim along the
        // tangent" sentinel, so a model that happens to sit on the origin gets
        // a negligible lift instead of a sideways camera.
        glm::vec3 target = pivot;
        if (glm::dot(target, target) < 1e-6f) target.y = 0.01f * radius;
        for (int i = 0; i < 8; ++i) {
            const float angle =
                static_cast<float>(i) / 8.0f * 2.0f * std::numbers::pi_v<float>;
            orbit.keys.push_back({pivot + glm::vec3{std::cos(angle) * 2.2f * radius,
                                                    0.35f * radius,
                                                    std::sin(angle) * 2.2f * radius},
                                  target});
        }
        result.paths.push_back(std::move(orbit));
        return result;
    };

    scene.unload = [session, state](SceneContext& ctx) {
        UnloadSession(ctx, *session);
        state->open_index = -1;
    };

    scene.update = [session, state](SceneContext& ctx, float delta_time) {
        if (auto outcome = PollDownload(ctx, *session);
            outcome.has_value() && outcome->ok) {
            const auto& catalog = session->downloader->Catalog();
            const auto found =
                std::ranges::find_if(catalog, [&outcome](const auto& asset) {
                    return asset.id == outcome->asset_id;
                });
            if (found != catalog.end()) {
                state->selected = static_cast<int>(
                    std::distance(catalog.begin(), found));
            }
            return;
        }
        if (ConsumeSessionReload(ctx, *session)) return;
        UpdateSession(ctx, *session, delta_time);
    };

    scene.options = [session, state](OptionsUi& ui) {
        ui.Section("Library");
        ui.Dynamic([session, state](OptionsUi& dynamic) {
            BuildLibrarySection(dynamic, *session, *state);
        });
        BuildSessionOptions(ui, *session);
    };

    return scene;
}

// ── procedural material lab ─────────────────────────────────────────────

struct LabState {
    float metallic = 0.35f;
    float roughness = 0.35f;
    float emissive_strength = 0.0f;
    bool spin = true;
    float spin_speed = 12.0f;
    float spin_angle = 0.0f;

    glm::vec3 sun_color{1.0f, 0.95f, 0.88f};
    float sun_intensity = 3.0f;
    float ambient = 0.06f;
    float sun_azimuth = 35.0f;
    float sun_elevation = 45.0f;
    bool lighting_dirty = true;
    float applied_sun_intensity = -1.0f;
    float applied_ambient = -1.0f;
    float applied_azimuth = 0.0f;
    float applied_elevation = 0.0f;
    glm::vec3 applied_sun_color{-1.0f};

    float applied_metallic = -1.0f;
    float applied_roughness = -1.0f;
    float applied_emissive = -1.0f;

    EntityId root_entity{};
    std::uint32_t mesh_id = kInvalidMesh;
    std::vector<MaterialHandle<DefaultMeshTechnique>> handles{};
    std::vector<DefaultMeshPerMaterialData> base_data{};
    TextureCache textures{};
};

[[nodiscard]] VulkanEngine::SceneLoader::LoadedMeshData BuildSphereGrid(
    SceneContext& ctx, LabState& state) {
    using namespace VulkanEngine;

    SceneLoader::LoadedMeshData mesh{};
    constexpr int kSegments = 32;
    constexpr int kRings = 20;
    constexpr float kRadius = 0.42f;
    constexpr int kGrid = 5;
    const float spacing = kRadius * 2.7f;

    std::vector<float> sphere_positions{};
    std::vector<float> sphere_normals{};
    std::vector<float> sphere_uvs{};
    std::vector<std::uint32_t> sphere_indices{};

    for (int ring = 0; ring <= kRings; ++ring) {
        const float v = static_cast<float>(ring) / static_cast<float>(kRings);
        const float phi = v * std::numbers::pi_v<float>;
        const float y = std::cos(phi);
        const float r = std::sin(phi);
        for (int seg = 0; seg <= kSegments; ++seg) {
            const float u = static_cast<float>(seg) / static_cast<float>(kSegments);
            const float theta = u * 2.0f * std::numbers::pi_v<float>;
            const float x = r * std::cos(theta);
            const float z = r * std::sin(theta);
            sphere_positions.insert(sphere_positions.end(),
                                    {x * kRadius, y * kRadius, z * kRadius});
            sphere_normals.insert(sphere_normals.end(), {x, y, z});
            sphere_uvs.insert(sphere_uvs.end(), {u, 1.0f - v});
        }
    }
    for (int ring = 0; ring < kRings; ++ring) {
        for (int seg = 0; seg < kSegments; ++seg) {
            const auto a = static_cast<std::uint32_t>(ring * (kSegments + 1) + seg);
            const auto b = static_cast<std::uint32_t>(a + kSegments + 1);
            // Counter-clockwise when seen from outside, matching Vulkan's front face.
            sphere_indices.insert(sphere_indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    }

    const auto vertex_count_per_sphere = static_cast<std::uint32_t>(sphere_positions.size() / 3);
    for (int gy = 0; gy < kGrid; ++gy) {
        for (int gx = 0; gx < kGrid; ++gx) {
            const auto cell = static_cast<std::uint32_t>(gy * kGrid + gx);
            const glm::vec3 center{(static_cast<float>(gx) - 2.0f) * spacing, 0.0f,
                                   (static_cast<float>(gy) - 2.0f) * spacing};

            const auto vertex_base = static_cast<std::uint32_t>(mesh.positions.size() / 3);
            for (std::uint32_t i = 0; i < vertex_count_per_sphere; ++i) {
                mesh.positions.push_back(sphere_positions[i * 3 + 0] + center.x);
                mesh.positions.push_back(sphere_positions[i * 3 + 1] + center.y);
                mesh.positions.push_back(sphere_positions[i * 3 + 2] + center.z);
                mesh.normals.push_back(sphere_normals[i * 3 + 0]);
                mesh.normals.push_back(sphere_normals[i * 3 + 1]);
                mesh.normals.push_back(sphere_normals[i * 3 + 2]);
                mesh.uvs.push_back(sphere_uvs[i * 2 + 0]);
                mesh.uvs.push_back(sphere_uvs[i * 2 + 1]);
            }

            SubMesh submesh{};
            submesh.index_start = static_cast<std::uint32_t>(mesh.indices.size());
            for (const std::uint32_t index : sphere_indices) {
                mesh.indices.push_back(vertex_base + index);
            }
            submesh.index_count = static_cast<std::uint32_t>(mesh.indices.size()) -
                                  submesh.index_start;
            submesh.sphere.center = {center.x, center.y, center.z};
            submesh.sphere.radius = kRadius * 1.1f;
            // The occlusion cull reads the OBB, so a zero-extent default would
            // make every sphere an occluder candidate with no volume.
            submesh.obb.center = {center.x, center.y, center.z};
            submesh.obb.axis_u = {1.0f, 0.0f, 0.0f};
            submesh.obb.half_extent_u = kRadius;
            submesh.obb.axis_v = {0.0f, 1.0f, 0.0f};
            submesh.obb.half_extent_v = kRadius;
            submesh.obb.axis_w = {0.0f, 0.0f, 1.0f};
            submesh.obb.half_extent_w = kRadius;
            ComputeSubmeshVertexWindow(submesh, mesh.indices);

            const glm::vec3 color =
                HsvToRgb(static_cast<float>(cell) / (kGrid * kGrid), 0.55f, 0.85f);
            const std::uint32_t albedo = state.textures.Solid(ctx, color);

            DefaultMeshPerMaterialData data{};
            data.albedo_texture = albedo;
            data.roughness_factor = state.roughness;
            data.metallic_factor = state.metallic;
            data.ao_factor = 1.0f;
            data.emissive_factor[3] = state.emissive_strength;

            auto handle = ctx.engine->GetContext().GetMaterialManager().Register<
                DefaultMeshTechnique>(MaterialDesc{.blend = MaterialManager::BlendMode::Opaque},
                                      data);
            if (handle.Valid()) {
                submesh.material_id = MaterialId{handle.Id()};
                state.handles.push_back(handle);
                state.base_data.push_back(data);
            } else {
                submesh.material_id = MaterialId{0};
            }
            mesh.submeshes.push_back(submesh);
        }
    }
    return mesh;
}

void UploadLabLighting(SceneContext& ctx, const LabState& state) {
    using namespace VulkanEngine;
    const glm::vec3 toward_sun =
        Examples::Explorer::DirectionFromAngles(state.sun_azimuth, state.sun_elevation);
    const glm::vec3 direction = -toward_sun;

    SceneRenderer::SceneHeader header{};
    header.ambient_color[0] = state.ambient;
    header.ambient_color[1] = state.ambient;
    header.ambient_color[2] = state.ambient;
    header.ambient_color[3] = 1.0f;
    header.sun_direction[0] = direction.x;
    header.sun_direction[1] = direction.y;
    header.sun_direction[2] = direction.z;
    header.sun_color[0] = state.sun_color.r;
    header.sun_color[1] = state.sun_color.g;
    header.sun_color[2] = state.sun_color.b;
    header.sun_color[3] = state.sun_intensity;
    header.light_count = 1;

    SceneRenderer::Light sun{};
    sun.direction[0] = direction.x;
    sun.direction[1] = direction.y;
    sun.direction[2] = direction.z;
    sun.color[0] = state.sun_color.r;
    sun.color[1] = state.sun_color.g;
    sun.color[2] = state.sun_color.b;
    sun.color[3] = state.sun_intensity;
    sun.position[3] = 0.0f;  // directional

    ctx.engine->UploadLighting(header, std::span<const SceneRenderer::Light>{&sun, 1});
}

[[nodiscard]] SceneDefinition MakeMaterialLabScene(std::shared_ptr<LabState> state) {
    SceneDefinition scene{};
    scene.id = "material-lab";
    scene.name = "Material Lab";
    scene.summary =
        "Procedural 5x5 sphere grid with live metallic/roughness/emissive overrides and a "
        "controllable sun. Needs no asset files, and ships two camera paths.";
    scene.license = "procedural";
    scene.attribution = "Generated by the explorer example.";

    scene.load = [state](SceneContext& ctx) -> SceneBuildResult {
        SceneBuildResult result{};
        auto loaded = BuildSphereGrid(ctx, *state);
        if (loaded.submeshes.empty()) {
            result.ok = false;
            result.error = "Failed to build the sphere grid.";
            return result;
        }
        state->handles.clear();
        state->base_data.clear();
        const std::uint32_t mesh_id =
            ctx.engine->GetMeshRegistry().Register(VulkanEngine::SceneLoader::ToMeshData(loaded));
        state->mesh_id = mesh_id;
        state->root_entity = ctx.CreateMeshEntity(mesh_id);
        result.entities.push_back(state->root_entity);
        const float extent = 5.0f * 0.42f * 2.7f * 0.5f + 0.5f;
        result.bounds =
            Bounds{glm::vec3{-extent, -0.5f, -extent}, glm::vec3{extent, 0.5f, extent}, true};
        state->applied_metallic = -1.0f;
        state->applied_roughness = -1.0f;
        state->applied_emissive = -1.0f;
        UploadLabLighting(ctx, *state);
        return result;
    };

    scene.unload = [state](SceneContext& ctx) {
        auto& material_manager = ctx.engine->GetContext().GetMaterialManager();
        for (auto& handle : state->handles) {
            if (handle.Valid()) material_manager.Destroy(MaterialId{handle.Id()});
        }
        state->handles.clear();
        state->base_data.clear();
        if (state->mesh_id != kInvalidMesh) {
            ctx.engine->GetMeshRegistry().Release(state->mesh_id, ctx.engine->GetMeshManager());
            state->mesh_id = kInvalidMesh;
        }
        state->applied_metallic = -1.0f;
        state->applied_roughness = -1.0f;
        state->applied_emissive = -1.0f;
    };

    scene.update = [state](SceneContext& ctx, float delta_time) {
        auto& registry = ctx.Registry();
        if (auto* entity = registry.TryGetEntity(state->root_entity); entity != nullptr) {
            if (auto* transform = entity->GetComponent<VulkanEngine::Components::Transform>();
                transform != nullptr) {
                if (state->spin) {
                    state->spin_angle =
                        std::fmod(state->spin_angle + state->spin_speed * delta_time, 360.0f);
                }
                transform->rotation = glm::angleAxis(glm::radians(state->spin_angle),
                                                     glm::vec3{0.0f, 1.0f, 0.0f});
            }
        }

        const bool lighting_dirty = state->lighting_dirty ||
                                    state->applied_sun_intensity != state->sun_intensity ||
                                    state->applied_ambient != state->ambient ||
                                    state->applied_azimuth != state->sun_azimuth ||
                                    state->applied_elevation != state->sun_elevation ||
                                    state->applied_sun_color != state->sun_color;
        if (lighting_dirty) {
            UploadLabLighting(ctx, *state);
            state->lighting_dirty = false;
            state->applied_sun_intensity = state->sun_intensity;
            state->applied_ambient = state->ambient;
            state->applied_azimuth = state->sun_azimuth;
            state->applied_elevation = state->sun_elevation;
            state->applied_sun_color = state->sun_color;
        }

        const bool materials_dirty = state->applied_metallic != state->metallic ||
                                     state->applied_roughness != state->roughness ||
                                     state->applied_emissive != state->emissive_strength;
        if (!materials_dirty) return;

        for (std::size_t i = 0; i < state->handles.size(); ++i) {
            auto& handle = state->handles[i];
            if (!handle.Valid()) continue;
            const float metallic = state->metallic;
            const float roughness = state->roughness;
            const float emissive = state->emissive_strength;
            handle.Modify<DefaultMeshPerMaterialData>(
                [metallic, roughness, emissive](DefaultMeshPerMaterialData& data) {
                    data.metallic_factor = metallic;
                    data.roughness_factor = roughness;
                    data.emissive_factor[3] = emissive;
                });
        }
        state->applied_metallic = state->metallic;
        state->applied_roughness = state->roughness;
        state->applied_emissive = state->emissive_strength;
    };

    scene.options = [state](OptionsUi& ui) {
        ui.Section("Material");
        ui.SliderFloat("Metallic", state->metallic, 0.0f, 1.0f, "%.2f");
        ui.SliderFloat("Roughness", state->roughness, 0.0f, 1.0f, "%.2f");
        ui.SliderFloat("Emissive strength", state->emissive_strength, 0.0f, 10.0f, "%.2f");

        ui.Section("Animation");
        ui.Checkbox("Spin", state->spin);
        ui.SliderFloat("Spin speed", state->spin_speed, -120.0f, 120.0f, "%.0f deg/s");

        ui.Section("Sun");
        ui.Color3("Color", state->sun_color);
        ui.SliderFloat("Intensity", state->sun_intensity, 0.0f, 12.0f, "%.2f");
        ui.SliderFloat("Azimuth", state->sun_azimuth, -180.0f, 180.0f, "%.0f deg");
        ui.SliderFloat("Elevation", state->sun_elevation, -5.0f, 89.0f, "%.0f deg");
        ui.SliderFloat("Ambient", state->ambient, 0.0f, 0.5f, "%.3f");
        ui.Button("Reset lighting", [state] {
            state->sun_color = glm::vec3{1.0f, 0.95f, 0.88f};
            state->sun_intensity = 3.0f;
            state->sun_azimuth = 35.0f;
            state->sun_elevation = 45.0f;
            state->ambient = 0.06f;
            state->lighting_dirty = true;
        });
    };

    CameraPath orbit{};
    orbit.name = "Orbit";
    orbit.closed = true;
    for (int i = 0; i < 8; ++i) {
        const float angle = static_cast<float>(i) / 8.0f * 2.0f * std::numbers::pi_v<float>;
        orbit.keys.push_back(
            {{std::cos(angle) * 6.5f, 2.6f, std::sin(angle) * 6.5f}, {0.0f, 0.1f, 0.0f}});
    }
    scene.paths.push_back(orbit);

    CameraPath sweep{};
    sweep.name = "Sweep";
    sweep.keys = {
        {{-7.5f, 1.8f, -7.5f}, {0.0f, 0.1f, 0.0f}},
        {{-3.0f, 1.2f, -3.0f}, {0.0f, 0.1f, 0.0f}},
        {{3.0f, 1.2f, 3.0f}, {0.0f, 0.1f, 0.0f}},
        {{7.5f, 1.8f, 7.5f}, {0.0f, 0.1f, 0.0f}},
    };
    scene.paths.push_back(sweep);

    CameraPath top{};
    top.name = "Top-down";
    top.keys = {
        {{0.0f, 9.0f, 0.2f}, {0.0f, 0.0f, 0.0f}},
        {{0.0f, 6.0f, 3.5f}, {0.0f, 0.0f, 0.0f}},
        {{0.0f, 3.5f, 5.5f}, {0.0f, 0.0f, 0.0f}},
    };
    scene.paths.push_back(top);

    return scene;
}

} // namespace

std::vector<Examples::Explorer::DownloadableAsset> BuildDownloadCatalog() {
    // The rows come from examples/scene_explorer/catalog.json, turned into this
    // header at configure time. That keeps the panel, `--list-downloads` and the
    // deployed credits/ folder from ever disagreeing about an entry, its
    // license or its checksum. Only CC0 and CC BY entries belong in the catalog:
    // Examples.Explorer.Download rejects everything else at run time, and the
    // generator rejects it at configure time.
    std::vector<Examples::Explorer::DownloadableAsset> catalog;
    catalog.reserve(SceneExplorerCatalog::kEntryCount);
    for (const auto& entry : SceneExplorerCatalog::kEntries) {
        DownloadableAsset asset{};
        asset.id = entry.id;
        asset.name = entry.name;
        asset.category = entry.category;
        asset.summary = entry.summary;
        asset.license = entry.license;
        asset.attribution = entry.attribution;
        asset.source_page = entry.source_page;
        asset.url = entry.url;
        asset.file_name = entry.file_name;
        asset.size = entry.size;
        asset.sha256 = entry.sha256;
        catalog.push_back(std::move(asset));
    }
    return catalog;
}

std::vector<std::size_t> FilterCatalog(
    std::span<const Examples::Explorer::DownloadableAsset> catalog, std::string_view query,
    std::string_view license, std::string_view category) {
    std::vector<std::size_t> matches{};
    const std::string needle = LowerText(query);
    for (std::size_t index = 0; index < catalog.size(); ++index) {
        const Examples::Explorer::DownloadableAsset& asset = catalog[index];
        if (!license.empty() && asset.license != license) continue;
        if (!category.empty() && asset.category != category) continue;
        if (!needle.empty()) {
            // Name, summary, attribution and category are what a user would
            // search by; the id and URLs stay out so "github" does not match
            // everything.
            const std::string haystack =
                LowerText(asset.name + "\n" + asset.summary + "\n" + asset.attribution +
                          "\n" + asset.category);
            if (haystack.find(needle) == std::string::npos) continue;
        }
        matches.push_back(index);
    }
    return matches;
}

Examples::Explorer::SceneCatalog BuildCatalog(const Examples::Explorer::CatalogContext& context) {
    // A hand-copied executable is the one case the build cannot cover. The
    // credits folder is text that names every downloaded asset's license and
    // attribution, so losing it is worth a warning even though the program
    // itself runs fine.
    if (!context.exe_dir.empty()) {
        std::error_code ec{};
        if (!std::filesystem::is_regular_file(context.exe_dir / "credits" / "CREDITS.md", ec)) {
            LOGIFACE_LOG(warn, "SceneExplorer: no credits/CREDITS.md next to the executable; "
                                   "the deployed asset licenses and attribution are missing");
        }
    }

    // One session for every scene that can show a downloaded asset. Two
    // sessions would mean two downloaders over one cache root, so two
    // transfers could truncate each other's ".part" file, and `--download`
    // would no longer be one-shot per process.
    auto session = std::make_shared<AssetSession>();
    session->asset_dir = context.asset_dir;
    session->files = DiscoverAssets(context.asset_dir);
    for (const auto& file : context.asset_files) {
        if (!std::filesystem::is_regular_file(file)) {
            LOGIFACE_LOG(warn, "SceneExplorer: --asset '" + file.string() + "' does not exist");
            continue;
        }
        if (std::ranges::find(session->files, file) == session->files.end()) {
            session->files.push_back(file);
        }
    }
    std::ranges::sort(session->files);

    Examples::Explorer::SceneCatalog catalog{};
    catalog.push_back(MakeSampleAssetsScene(session));
    catalog.push_back(MakeAssetLibraryScene(session, std::make_shared<LibraryState>()));
    catalog.push_back(MakeMaterialLabScene(std::make_shared<LabState>()));
    return catalog;
}

} // namespace Examples::SceneExplorer
