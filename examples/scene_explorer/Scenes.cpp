module;

#include <cctype>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

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

// Pinned commit of KhronosGroup/glTF-Sample-Assets. These URLs must never point
// at a branch: the SHA-256 below only means something if the bytes cannot
// change underneath it.
constexpr const char* kGltfSampleAssetsCommit = "8f492390ddc7a2ae221a1369ef560589cf8c70fb";

[[nodiscard]] std::string PinnedAssetUrl(std::string_view model, std::string_view file) {
    return std::string{"https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/"} +
           kGltfSampleAssetsCommit + "/Models/" + std::string{model} + "/glTF-Binary/" +
           std::string{file};
}

[[nodiscard]] std::string SourcePage(std::string_view model) {
    return "https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/" +
           std::string{model};
}

[[nodiscard]] std::string FormatByteSize(std::uint64_t bytes) {
    if (bytes >= 1024ull * 1024ull) {
        return std::format("{:.1f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    if (bytes >= 1024ull) {
        return std::format("{:.1f} KiB", static_cast<double>(bytes) / 1024.0);
    }
    return std::format("{} B", bytes);
}

[[nodiscard]] DownloadableAsset MakeDownloadable(std::string id, std::string name,
                                                 std::string summary, std::string license,
                                                 std::string attribution, std::string model,
                                                 std::uint64_t size, std::string sha256) {
    DownloadableAsset asset{};
    asset.id = std::move(id);
    asset.name = std::move(name);
    asset.summary = std::move(summary);
    asset.license = std::move(license);
    asset.attribution = std::move(attribution);
    asset.source_page = SourcePage(model);
    asset.url = PinnedAssetUrl(model, model + ".glb");
    asset.file_name = asset.id + ".glb";
    asset.size = size;
    asset.sha256 = std::move(sha256);
    return asset;
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

struct SampleAssetsState {
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
    // root only exists once the engine's per-user storage is up.
    std::unique_ptr<SceneDownloader> downloader{};
    std::vector<DownloadableAsset> library{};
    int library_index = 0;
    std::string download_message{};
    std::filesystem::path download_root{};
    bool auto_download_started = false;

    GltfAsset* active_asset = nullptr;
    Bounds active_bounds{};
    EntityId root_entity{};
};

void EnsureDownloader(SceneContext& ctx, SampleAssetsState& state) {
    if (state.downloader) return;
    if (ctx.app == nullptr || ctx.app->storage == nullptr) {
        state.download_message =
            "Per-user storage is unavailable, so on-demand downloads are disabled.";
        return;
    }
    state.download_root = ctx.app->storage->CacheDir() / "scenes";
    state.library = BuildDownloadCatalog();
    state.downloader = std::make_unique<SceneDownloader>(state.download_root);
    state.downloader->SetCatalog(state.library);
}

// Local assets plus everything already downloaded. Sorted and deduplicated, so
// the combo order is stable across frames.
[[nodiscard]] std::vector<std::filesystem::path> CollectAssetFiles(SampleAssetsState& state) {
    std::vector<std::filesystem::path> files = DiscoverAssets(state.asset_dir);
    if (state.downloader) {
        for (const auto& downloaded : state.downloader->InstalledFiles()) {
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
void ApplyModelTransform(SceneContext& ctx, const SampleAssetsState& state) {
    auto& registry = ctx.Registry();
    auto* entity = registry.TryGetEntity(state.root_entity);
    if (entity == nullptr) return;
    auto* transform = entity->GetComponent<VulkanEngine::Components::Transform>();
    if (transform == nullptr) return;

    const glm::vec3 center = state.active_bounds.Center() * state.scale;
    const glm::quat rotation =
        glm::angleAxis(glm::radians(state.spin_angle), glm::vec3{0.0f, 1.0f, 0.0f});
    transform->scale = glm::vec3{state.scale};
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
void ApplyUnlit(SceneContext& ctx, SampleAssetsState& state, bool enabled) {
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

[[nodiscard]] SceneDefinition MakeSampleAssetsScene(std::shared_ptr<SampleAssetsState> state) {
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

    scene.load = [state](SceneContext& ctx) -> SceneBuildResult {
        SceneBuildResult result{};
        EnsureDownloader(ctx, *state);
        state->files = CollectAssetFiles(*state);

        if (!ctx.initial_action.empty() && !state->auto_download_started) {
            state->auto_download_started = true;
            if (state->downloader == nullptr) {
                state->download_message = "Downloads are unavailable.";
            } else {
                const auto found = std::ranges::find_if(state->library, [&ctx](const auto& asset) {
                    return asset.id == ctx.initial_action;
                });
                if (found == state->library.end()) {
                    state->download_message = "Unknown --download id '" + ctx.initial_action + "'.";
                    LOGIFACE_LOG(warn, "SceneExplorer: " + state->download_message);
                } else if (state->downloader->IsInstalled(*found)) {
                    state->download_message = found->name + " is already installed.";
                    LOGIFACE_LOG(info, "SceneExplorer: " + state->download_message);
                } else if (state->downloader->Start(*found)) {
                    state->download_message = "Downloading " + found->name + "...";
                    LOGIFACE_LOG(info, "SceneExplorer: " + state->download_message);
                } else {
                    state->download_message = "Could not start the download.";
                    LOGIFACE_LOG(warn, "SceneExplorer: " + state->download_message);
                }
            }
        }

        if (state->files.empty()) {
            const bool downloading =
                state->downloader != nullptr &&
                state->downloader->Progress().state == DownloadState::Running;
            result.ok = downloading;
            if (downloading) {
                result.notice = state->download_message;
            } else {
                result.error =
                    "No .glb/.gltf files found in '" + state->asset_dir.string() +
                    "'. Pass --assets <dir>, download one in the panel, or use --download <id>.";
            }
            return result;
        }
        state->index = std::clamp(state->index, 0, static_cast<int>(state->files.size()) - 1);
        const std::filesystem::path path = state->files[static_cast<std::size_t>(state->index)];

        GltfAsset* asset = nullptr;
        if (const auto it = state->cache.find(path.string()); it != state->cache.end()) {
            asset = &it->second;
        } else {
            std::string error{};
            GltfAsset loaded{};
            if (!LoadGltfAsset(ctx, path, state->textures, loaded, error) ||
                loaded.mesh_id == kInvalidMesh) {
                result.ok = false;
                result.error = error.empty() ? "The asset produced no mesh." : error;
                // Remember the attempt so the update loop does not re-import (and
                // re-log) the same broken file every frame; Reload retries it.
                state->loaded_index = state->index;
                state->active_asset = nullptr;
                return result;
            }
            asset = &state->cache.emplace(path.string(), std::move(loaded)).first->second;
        }

        state->active_asset = asset;
        state->active_bounds = asset->bounds;
        state->loaded_index = state->index;
        state->spin_angle = 0.0f;
        state->override_applied = false;
        state->root_entity = ctx.CreateMeshEntity(asset->mesh_id);
        if (auto* root = ctx.Registry().TryGetEntity(state->root_entity)) {
            auto& override_comp =
                ctx.Registry().AddComponent<VulkanEngine::Components::MaterialOverride>(*root);
            override_comp.mesh_id = asset->mesh_id;
        }
        state->unlit_applied = false;
        result.entities.push_back(state->root_entity);
        result.bounds = ScaledBounds(asset->bounds, state->scale);
        ApplyModelTransform(ctx, *state);
        return result;
    };

    scene.unload = [state](SceneContext& ctx) {
        state->active_asset = nullptr;
        for (auto& [path, asset] : state->cache) {
            (void)path;
            ReleaseGltfAsset(ctx, asset);
        }
        state->cache.clear();
        state->loaded_index = -1;
    };

    scene.update = [state](SceneContext& ctx, float delta_time) {
        if (state->unlit != state->unlit_applied && state->active_asset != nullptr) {
            ApplyUnlit(ctx, *state, state->unlit);
            state->unlit_applied = state->unlit;
        }
        if (state->downloader != nullptr) {
            if (auto outcome = state->downloader->TakeOutcome(); outcome.has_value()) {
                if (outcome->ok) {
                    state->download_message =
                        "Downloaded " + outcome->path.filename().string();
                    LOGIFACE_LOG(info, "SceneExplorer: downloaded '" + outcome->path.string() +
                                           "' (" + FormatByteSize(
                                               std::filesystem::file_size(outcome->path)) +
                                           ")");
                    state->files = CollectAssetFiles(*state);
                    const auto found = std::ranges::find(state->files, outcome->path);
                    if (found != state->files.end()) {
                        state->index = static_cast<int>(
                            std::distance(state->files.begin(), found));
                    }
                    state->loaded_index = -1;
                    ctx.RequestReload();
                    return;
                }
                state->download_message = "Download failed: " + outcome->error;
                LOGIFACE_LOG(warn, "SceneExplorer: " + state->download_message);
            }
        }

        if (state->rescan_requested) {
            state->rescan_requested = false;
            state->files = DiscoverAssets(state->asset_dir);
            state->index = state->files.empty() ? 0 : std::clamp(state->index, 0,
                                                                 static_cast<int>(state->files.size()) - 1);
            ctx.RequestReload();
            return;
        }
        if (state->reload_requested) {
            state->reload_requested = false;
            // An explicit reload means "re-read from disk": drop the cached
            // import for the current file so an edited asset is picked up.
            if (!state->files.empty()) {
                const auto index = static_cast<std::size_t>(
                    std::clamp(state->index, 0, static_cast<int>(state->files.size()) - 1));
                state->cache.erase(state->files[index].string());
                state->active_asset = nullptr;
                state->loaded_index = -1;
            }
            ctx.RequestReload();
            return;
        }
        if (state->files.empty()) return;
        if (state->index != state->loaded_index) {
            ctx.RequestReload();
            return;
        }

        if (state->spin) {
            state->spin_angle =
                std::fmod(state->spin_angle + state->spin_speed * delta_time, 360.0f);
        }
        ApplyModelTransform(ctx, *state);

        if (state->active_asset == nullptr) return;
        const auto apply = [&](const std::function<void(DefaultMeshPerMaterialData&)>& mutate) {
            for (auto& handle : state->active_asset->handles) {
                if (!handle.Valid()) continue;
                handle.Modify<DefaultMeshPerMaterialData>(mutate);
            }
        };

        if (state->override_factors) {
            const bool changed = !state->override_applied ||
                                 state->applied_roughness != state->roughness ||
                                 state->applied_metallic != state->metallic ||
                                 state->applied_emissive != state->emissive_strength;
            if (changed) {
                apply([state](DefaultMeshPerMaterialData& data) {
                    data.roughness_factor = state->roughness;
                    data.metallic_factor = state->metallic;
                    data.emissive_factor[3] = state->emissive_strength;
                });
                state->applied_roughness = state->roughness;
                state->applied_metallic = state->metallic;
                state->applied_emissive = state->emissive_strength;
                state->override_applied = true;
            }
        } else if (state->override_applied) {
            // Restore what the asset declared.
            const auto& base = state->active_asset->base_data;
            for (std::size_t i = 0; i < state->active_asset->handles.size() && i < base.size(); ++i) {
                auto& handle = state->active_asset->handles[i];
                if (!handle.Valid()) continue;
                const DefaultMeshPerMaterialData original = base[i];
                handle.Modify<DefaultMeshPerMaterialData>(
                    [original](DefaultMeshPerMaterialData& data) { data = original; });
            }
            state->override_applied = false;
        }
    };

    scene.options = [state](OptionsUi& ui) {
        ui.Section("Asset");
        if (state->files.empty()) {
            ui.TextDisabled("No glTF files discovered.");
            ui.TextWrapped("Pass --assets <dir> at launch, or --asset <file> (repeatable), then "
                           "press Rescan.");
        } else {
            std::vector<std::string> labels{};
            labels.reserve(state->files.size());
            for (const auto& file : state->files) labels.push_back(AssetLabel(file));
            std::vector<std::string_view> views{};
            views.reserve(labels.size());
            for (const auto& label : labels) views.push_back(label);
            ui.Combo("Asset", state->index, views, "Switching rebuilds the scene.");
            ui.TextDisabled(std::to_string(state->index + 1) + " / " +
                            std::to_string(state->files.size()) + " files");
        }
        ui.Button("Rescan directory", [state] { state->rescan_requested = true; });
        ui.SameLine();
        ui.Button("Reload asset", [state] { state->reload_requested = true; });

        ui.Section("Transform");
        ui.Checkbox("Spin", state->spin);
        ui.SliderFloat("Spin speed", state->spin_speed, -180.0f, 180.0f, "%.0f deg/s");
        ui.SliderFloat("Scale", state->scale, 0.01f, 20.0f, "%.2f");

        ui.Section("Shading");
        ui.Checkbox("Unlit", state->unlit,
                    "Sample the albedo texture only: no lights, normal map or ORM.");

        ui.Section("Material override");
        ui.Checkbox("Override factors", state->override_factors,
                    "Applies one set of factors to every lit material of this asset.");
        if (state->override_factors) {
            ui.SliderFloat("Roughness", state->roughness, 0.0f, 1.0f, "%.2f");
            ui.SliderFloat("Metallic", state->metallic, 0.0f, 1.0f, "%.2f");
            ui.SliderFloat("Emissive strength", state->emissive_strength, 0.0f, 10.0f, "%.2f");
        }

        ui.Section("Download");
        ui.Dynamic([state](OptionsUi& download) {
            if (state->downloader == nullptr) {
                download.TextWrapped(state->download_message.empty()
                                         ? "On-demand downloads are unavailable."
                                         : state->download_message);
                return;
            }

            SceneDownloader& downloader = *state->downloader;
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

            state->library_index = std::clamp(state->library_index, 0,
                                              static_cast<int>(catalog.size()) - 1);
            download.Combo("Asset library", state->library_index, views,
                           "Fetched on demand into the per-user cache; nothing is bundled.");

            const DownloadableAsset& asset =
                catalog[static_cast<std::size_t>(state->library_index)];
            download.TextDisabled(asset.license + "  ·  " + FormatByteSize(asset.size));
            download.TextWrapped(asset.summary);
            download.TextDisabled(asset.attribution);

            const auto progress = downloader.Progress();
            const bool installed = downloader.IsInstalled(asset);
            const bool running_this =
                progress.state == DownloadState::Running && progress.asset_id == asset.id;

            if (installed) {
                download.TextDisabled("Installed in the cache.");
                download.Button("Remove downloaded asset", [state, asset] {
                    if (state->downloader->Remove(asset)) {
                        state->download_message = "Removed " + asset.name + " from the cache.";
                        state->rescan_requested = true;
                    }
                });
            } else if (running_this) {
                download.ProgressBar(progress.Fraction(),
                                     FormatByteSize(progress.bytes_received) + " / " +
                                         FormatByteSize(progress.bytes_total));
                download.Button("Cancel download", [state] { state->downloader->Cancel(); });
            } else {
                download.Button("Download and open", [state, asset] {
                    if (state->downloader->Start(asset)) {
                        state->download_message = "Downloading " + asset.name + "...";
                    } else {
                        state->download_message =
                            "Another download is already running (one at a time).";
                    }
                });
            }

            if (progress.state == DownloadState::Running && !running_this &&
                !progress.asset_id.empty()) {
                download.TextDisabled("Downloading " + progress.asset_id + "...");
            }
            if (!state->download_message.empty()) {
                download.TextWrapped(state->download_message);
            }
        });
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
    // Licenses are the ones the upstream README/LICENSE declares; only CC0 and
    // CC BY 4.0 entries belong here (see Examples.Explorer.Download's policy).
    return {
        MakeDownloadable("toy-car", "Toy Car",
                         "Toy car with transmission, clearcoat and sheen.", "CC0-1.0",
                         "Guido Odendahl (model); Eric Chadwick (extensions, scene). CC0 1.0.",
                         "ToyCar", 5422412,
                         "01a60862de55cd4b9f3acfab0b0def86451800f9c42467fcd61052c16cb9838c"),
        MakeDownloadable("sheen-chair", "Sheen Chair",
                         "Chair using material variants and sheen.", "CC0-1.0",
                         "Eric Chadwick / Wayfair. CC0 1.0.", "SheenChair", 4125648,
                         "f0af2a2b102d28d540236306ae19f8fb36842df76bd38cf76f063f9bd2853399"),
        MakeDownloadable("iridescence-lamp", "Iridescence Lamp",
                         "Wayfair lamp with transmission, volume and iridescence.", "CC-BY-4.0",
                         "Eric Chadwick / Wayfair. CC BY 4.0.", "IridescenceLamp", 4083912,
                         "17cfdc79ec9e38fadb28db159734c5f9592ca423977b546e5057e350ba29ff13"),
        MakeDownloadable("commercial-refrigerator", "Commercial Refrigerator",
                         "Refrigerator with glass condensation, bottles and a door animation.",
                         "CC-BY-4.0",
                         "Eric Chadwick / Darmstadt Graphics Group; based on 'Commercial Fridge' "
                         "by Sean Thomas. CC BY 4.0.",
                         "CommercialRefrigerator", 10131180,
                         "ef8da8b144e650c277ac953e6d9e50ac1689ff1ee88c2c5bd9551ae8bccb6065"),
        MakeDownloadable("mosquito-in-amber", "Mosquito in Amber",
                         "Mosquito in amber: transmission, IOR and volume.", "CC-BY-4.0",
                         "Loic Norgeot; real-time refraction by Sketchfab. CC BY 4.0.",
                         "MosquitoInAmber", 24229904,
                         "1c0b49000650e8a8c00d69bc8d64e46d33527871a47d74e4f2278887325cc35f"),
        MakeDownloadable("a-beautiful-game", "A Beautiful Game",
                         "Chess set using transmission and volume. Largest entry (33 images); "
                         "slowest to open on software renderers.",
                         "CC-BY-4.0",
                         "ASWF / MaterialX Project; glTF conversion by Ed Mackey. CC BY 4.0.",
                         "ABeautifulGame", 42977928,
                         "bd7133b4b322aae97c589b8839dae8155ad2546acb35ae32a127e722a959d007"),
    };
}

Examples::Explorer::SceneCatalog BuildCatalog(const Examples::Explorer::CatalogContext& context) {
    auto assets = std::make_shared<SampleAssetsState>();
    assets->asset_dir = context.asset_dir;
    assets->files = DiscoverAssets(context.asset_dir);
    for (const auto& file : context.asset_files) {
        if (!std::filesystem::is_regular_file(file)) {
            LOGIFACE_LOG(warn, "SceneExplorer: --asset '" + file.string() + "' does not exist");
            continue;
        }
        if (std::ranges::find(assets->files, file) == assets->files.end()) {
            assets->files.push_back(file);
        }
    }
    std::ranges::sort(assets->files);

    Examples::Explorer::SceneCatalog catalog{};
    catalog.push_back(MakeSampleAssetsScene(std::move(assets)));
    catalog.push_back(MakeMaterialLabScene(std::make_shared<LabState>()));
    return catalog;
}

} // namespace Examples::SceneExplorer
