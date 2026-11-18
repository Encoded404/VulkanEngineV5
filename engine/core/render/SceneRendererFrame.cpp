module;

#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/matrix_transform.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/quaternion.hpp> //NOLINT(misc-include-cleaner)

#include <logging/logging_macros.hpp>

module VulkanEngine.SceneRenderer;

import std;
import std.compat;

import logiface;

import vulkan;

import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.Components.Transform;
import VulkanEngine.Components.MeshReference;
import VulkanEngine.GpuResources;
import VulkanEngine.StandardMeshPipeline;
import VulkanEngine.TechniqueManager;
import VulkanEngine.BindlessManager;
import VulkanEngine.MaterialManager;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneGpuLayout;

namespace VulkanEngine::SceneRenderer {

// Short names for the slot enums. The tables own the numbering; these only keep the call sites readable.
using SetIdx = SceneGpuLayout::SetId;
using BufIdx = SceneGpuLayout::BufferSlot;
using BlkIdx = SceneGpuLayout::BlockSlot;

    namespace {
        struct ExpandPC {
            glm::mat4 vp;
            std::uint32_t cnt;
            std::uint32_t p0;
            std::uint32_t p1;
            // 0 keeps the expand pass off the skin-entry descriptor entirely.
            std::uint32_t hasActiveDeform;
            // Elements per deform region (T, not 2T).
            std::uint32_t regionSpan;
        };
        static_assert(sizeof(ExpandPC) == 84, "ExpandPC must match expand.slang");
        struct HiZPC { std::uint32_t bl; std::uint32_t sw; std::uint32_t sh; std::uint32_t tc; };
        // projInfo = (|proj[0][0]|, |proj[1][1]|, proj[2][2], 1 if perspective else 0)
        struct OccPC { std::uint32_t cnt; std::uint32_t refineLevel; std::uint32_t hizWidth; std::uint32_t hizHeight; glm::vec4 projInfo; };
        struct CollectPC { std::uint32_t cnt; std::uint32_t p0; std::uint32_t mt; std::uint32_t pass; };
        struct WritePC { std::uint32_t cnt; std::uint32_t p0; std::uint32_t techniqueCount; std::uint32_t p1; };
        struct OccluderSelectPC { std::uint32_t cnt; std::uint32_t minAreaPx; std::uint32_t screenW; std::uint32_t screenH; };
        struct PreCullPC { std::uint32_t cnt; std::uint32_t hizW; std::uint32_t hizH; std::uint32_t p0; glm::vec4 projInfo; };

        template<typename Handle>
        std::uint64_t HandleToU64(Handle h) {
            return reinterpret_cast<std::uint64_t>(static_cast<typename Handle::CType>(h));
        }

    } // anonymous namespace

void SceneRenderer::PrepareCompute(vk::CommandBuffer /*cmd*/,
                                    VulkanEngine::ComponentRegistry& /*reg*/,
                                    const glm::mat4& vm, const glm::mat4& pm,
                                    std::uint32_t, std::uint32_t, std::uint32_t fi) {
    PollShaders(fi);
    view_proj_ = pm * vm;
    const std::uint32_t f = fi % frames_in_flight_;
    auto& fr = frames_[f];

    // Projection coefficients for the occlusion culler. The x/y scales are the NDC
    // units per world unit (focal length for perspective, 1/half-extent for ortho),
    // z is the clip-space z scale, and the flag distinguishes the two: a perspective
    // projection derives w from view z (pm[3][2] = -1), an ortho projection keeps
    // w == 1 (pm[3][2] = 0).
    fr.proj_info = glm::vec4(
        std::abs(pm[0][0]), std::abs(pm[1][1]), pm[2][2],
        (pm[3][2] != 0.0f) ? 1.0f : 0.0f);

    const std::uint32_t total = current_entity_count_;
    if (total == 0) {
        LOGIFACE_LOG(trace, "PrepareCompute: current_entity_count_ is 0, no work to do");
    }

    // Grow the per-submesh block arrays the frame loop owns.
    // Each is bounded by Config::max_blocks (the descriptor binding array size), so a failure here means the scene exceeded the capacity budget; report once and let the caller clamp.
    // The rows say which pass grows which array: gather owns the SkinEntry table and grows it itself.
    const auto grow_blocks = [](GpuResources::BlockArray& buf, const std::uint32_t count,
                                const std::string_view name) {
        if (!buf.EnsureCapacity(count)) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                LOGIFACE_LOG(error, std::string("PrepareCompute: BlockArray '") + std::string(name) +
                    "' could not grow to hold " + std::to_string(count) +
                    " entries; it is capped at " + std::to_string(buf.BlockLimit()) +
                    " blocks");
            }
        }
    };
    for (const SceneGpuLayout::BlockArraySpec& spec : SceneGpuLayout::kBlockArrays) {
        if (spec.grown_by != SceneGpuLayout::GrownBy::frame_loop) continue;
        grow_blocks(fr[spec.slot], total, spec.name);
    }

    // The arrays grown here take the same `total` and the same entries-per-block, so they always share a block count, and this lockstep is the invariant the shaders' block lookups rely on.
    // The assertion walks the same rows as the growth loop, so it cannot fall behind it.
#ifndef NDEBUG
    const std::uint32_t block_count = fr[BlkIdx::dynamic_entries].BlockCount();
    for (const SceneGpuLayout::BlockArraySpec& spec : SceneGpuLayout::kBlockArrays) {
        if (spec.grown_by != SceneGpuLayout::GrownBy::frame_loop) continue;
        assert(fr[spec.slot].BlockCount() == block_count &&
               "the submesh block arrays must grow in lockstep");
    }
#endif

    const bool mid = draw_mode_ == DrawMode::MID;

    // Zero every buffer whose row asks for it.
    // The shaders append into these with atomics, so last frame's contents would be read as this frame's counts.
    for (const SceneGpuLayout::BufferSpec& spec : SceneGpuLayout::kFrameBuffers) {
        if (spec.clear == SceneGpuLayout::ClearMode::none) continue;
        if (spec.clear == SceneGpuLayout::ClearMode::cid_only && mid) continue;
        ClearBuffer(fr[spec.slot]);
    }

    LOGIFACE_LOG(trace, "PrepareCompute: binding dynamic_entries blocks=" +
                 std::to_string(fr[BlkIdx::dynamic_entries].BlockCount()) +
                 " static_entries blocks=" + std::to_string(fr[BlkIdx::static_entries].BlockCount()) +
                 " total=" + std::to_string(total));

    // Descriptor writes: one pass over the set table.
    // A row is written once per binding epoch; a block array publishes only the blocks that do not exist yet (see WriteBinding).
    for (const SceneGpuLayout::SetSpec& set : SceneGpuLayout::kSceneSets) {
        // The collect-write set is CID only: its buffers do not exist in MID, so the set is never bound or written there.
        if (set.cid_only && mid) continue;
        for (const SceneGpuLayout::BindingSpec& binding : set.bindings) {
            if (binding.writer != SceneGpuLayout::Writer::prepare_compute) continue;
            WriteBinding(fr, set, binding);
        }
    }
}

void SceneRenderer::SetTechniqueCommandRegions(
    std::span<const std::uint32_t> submeshes_per_technique) {
    region_base_.assign(MAX_DRAW_GROUPS, 0);
    region_count_.assign(MAX_DRAW_GROUPS, 0);
    std::uint32_t offset = 0;
    std::uint32_t highest = 0;
    const std::size_t n = std::min<std::size_t>(submeshes_per_technique.size(), MAX_DRAW_GROUPS);
    for (std::size_t t = 0; t < n; ++t) {
        region_base_[t] = offset;
        region_count_[t] = submeshes_per_technique[t];
        offset += submeshes_per_technique[t];
        if (submeshes_per_technique[t] != 0) highest = static_cast<std::uint32_t>(t) + 1;
    }
    region_total_ = offset;
    // The collect passes only need to scan keys that can appear; the highest
    // key with submeshes is the tight bound. UpdateTechniqueFlags may raise it
    // to the registered count before the dispatches run.
    technique_count_ = highest;

    if (!backend_) return;
    for (auto& fr : frames_) {
        if (fr[BufIdx::technique_region_bases].GetSize() >= region_base_.size() * sizeof(std::uint32_t)) {
            fr[BufIdx::technique_region_bases].Upload(region_base_.data(),
                region_base_.size() * sizeof(std::uint32_t));
        }
    }
}

void SceneRenderer::UpdateTechniqueFlags(
    VulkanEngine::TechniqueManager::TechniqueManager& tm) {
    if (!backend_) return;

    // Flags are per draw group, inheriting the owning technique's PipelineFlags.
    // The interning guarantee that every live group is below the registered
    // technique count is what lets the collect passes scan only up to that
    // count; assert it in debug builds.
    const std::uint32_t count =
        std::min<std::uint32_t>(tm.GetDrawGroupCount(), MAX_DRAW_GROUPS);
    if (tm.GetDrawGroupCount() > MAX_DRAW_GROUPS) {
        LOGIFACE_LOG(warn, "UpdateTechniqueFlags: " +
                     std::to_string(tm.GetDrawGroupCount()) +
                     " draw groups exceed flag-table capacity " +
                     std::to_string(MAX_DRAW_GROUPS));
    }
    // The live group count is the upper bound on any key, so the collect passes
    // never scan beyond it. Keep a tighter region bound when it is already
    // higher (region indices are group ids).
    technique_count_ = std::max(technique_count_, count);

    std::vector<std::uint32_t> flags(MAX_DRAW_GROUPS, 0);
    for (std::uint32_t g = 0; g < count; ++g) {
        const std::uint16_t tech_id = tm.GetDrawGroupTechnique(static_cast<std::uint16_t>(g));
        auto* tech = tm.GetTechnique(tech_id);
        if (!tech) continue;
        const auto& f = tech->pipeline_flags;
        const auto group = static_cast<std::uint16_t>(g);
        const std::uint32_t render_state = tm.GetDrawGroupRenderState(group);
        const std::uint32_t variant = tm.GetDrawGroupVariant(group);
        const auto coverage = tech->GetDepthCoverage(variant);
        std::uint32_t bits = 0;
        if (GroupWritesPrepassDepth(f.participates_in_depth_pass, render_state, coverage)) {
            bits |= TECHNIQUE_FLAG_DEPTH_PASS;
        }
        if (f.receives_occlusion)        bits |= TECHNIQUE_FLAG_RECEIVES_OCCLUSION;
        if (f.participates_in_collect)   bits |= TECHNIQUE_FLAG_COLLECT;
        if (f.bounds_conservative)       bits |= TECHNIQUE_FLAG_OCCLUDER_SAFE;
        flags[g] = bits;
    }
    SetTechniqueFlags(flags);
}

void SceneRenderer::SetTechniqueFlags(std::span<const std::uint32_t> flags) {
    if (!backend_) return;
    std::vector<std::uint32_t> padded(MAX_DRAW_GROUPS, 0);
    const std::size_t count = std::min(flags.size(), padded.size());
    std::copy_n(flags.begin(), static_cast<std::ptrdiff_t>(count), padded.begin());
    // Upload only when contents change (flags are static in practice).
    if (technique_flags_cache_ != padded) {
        technique_flags_cache_ = std::move(padded);
        owned_buffers_[BufIdx::technique_flags].Upload(
            technique_flags_cache_.data(),
            sizeof(std::uint32_t) * technique_flags_cache_.size());
    }
}

bool SceneRenderer::BindGroupDepthPipeline(
    vk::CommandBuffer cmd,
    VulkanEngine::TechniqueManager::BaseTechnique& tech,
    std::uint32_t draw_key,
    VulkanEngine::TechniqueManager::DepthCoverage coverage,
    vk::DescriptorSet bindless_set,
    const std::array<vk::DescriptorSet, 4>& engine_sets,
    vk::DescriptorSet scene_uniform_set,
    const float camera_pos[4]) {
    if (!GroupUsesCustomDepthPipeline(coverage)) {
        // EngineDepthOnly: the global pipeline ignores the technique's custom
        // sets, so bind sets 0-3 only.
        if (!depth_slot_.Get() || !layouts_[SceneGpuLayout::PipelineLayoutId::depth]) return false;
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, depth_slot_.Get());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, Layout(SceneGpuLayout::PipelineLayoutId::depth),
                               0, engine_sets, {});
        return true;
    }

    if (!tech.HasDepthVariant(draw_key) &&
        !tech.EnsureDepthVariant(draw_key, *shader_mgr_, *pipeline_factory_)) {
        return false;
    }
    auto pipeline = tech.GetDepthPipeline(draw_key);
    auto layout = tech.GetPipelineLayout();
    if (!pipeline || !layout) return false;

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    std::array<vk::DescriptorSet, 16> ds{};
    std::uint32_t slot = 0;
    ds[slot++] = bindless_set;        // set 0: bindless textures
    ds[slot++] = engine_sets[1];      // set 1: submesh vertex data
    ds[slot++] = engine_sets[2];      // set 2: raw vertex buffers
    ds[slot++] = engine_sets[3];      // set 3: indirection
    ds[slot++] = scene_uniform_set;   // set 4: scene uniforms
    for (const auto& tech_ds : tech.GetCustomDescriptorSets()) {
        ds[slot++] = tech_ds;
    }
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0,
                           vk::ArrayProxy<const vk::DescriptorSet>(slot, ds.data()), {});
    // Coverage fragment shaders may read the camera position. The technique
    // layout always declares this 16-byte range.
    cmd.pushConstants(layout, vk::ShaderStageFlagBits::eFragment, 0, 16, camera_pos);
    return true;
}

void SceneRenderer::DepthPrepass(vk::CommandBuffer cmd, std::uint32_t w, std::uint32_t h,
                                 std::uint32_t fi,
                                 VulkanEngine::TechniqueManager::TechniqueManager& tm,
                                 VulkanEngine::BindlessManager::BindlessManager& bm,
                                 const glm::mat4& view) {
    auto& fr = frames_[fi % frames_in_flight_];
    if (!depth_slot_.Get()) {
        LOGIFACE_LOG(warn, "DepthPrepass: depth pipeline is null, skipping");
        return;
    }
    if (!current_entity_count_) {
        LOGIFACE_LOG(trace, "DepthPrepass: current_entity_count_ is 0, skipping");
        return;
    }
    LOGIFACE_LOG(trace, "DepthPrepass: submesh_count=" + std::to_string(current_entity_count_) +
                 " (" + std::to_string(w) + "x" + std::to_string(h) + ")");
    cmd.setViewport(0, VulkanEngine::PipelinePass::SceneViewport(w, h));
    cmd.setScissor(0, vk::Rect2D({0, 0}, {w, h}));

    const bool mid = draw_mode_ == DrawMode::MID;
    if (mid) {
        cmd.bindIndexBuffer(*fr[BufIdx::draw_indices].GetBuffer(), 0, vk::IndexType::eUint32);
    } else {
        cmd.bindIndexBuffer(*fr[BufIdx::depth_indices].GetBuffer(), 0, vk::IndexType::eUint32);
    }

    // Engine descriptor sets, shared by the global depth pipeline and every
    // technique depth pipeline (the layouts agree on sets 0-4).
    const std::array<vk::DescriptorSet, 4> engine_sets{
        fr[SetIdx::empty].GetHandle(),
        fr[SetIdx::submesh_vertex].GetHandle(),
        fr[SetIdx::vertex_buffers].GetHandle(),
        fr[SetIdx::indirection].GetHandle()
    };
    const vk::DescriptorSet bindless_set = bm.GetDescriptorSet();
    const vk::DescriptorSet scene_uniform_set = GetSceneUniformSet();
    const glm::mat4 inv_view = glm::inverse(view);
    float camera_pos[4] = {inv_view[3][0], inv_view[3][1], inv_view[3][2], 0.0f};

    const std::uint32_t group_count =
        std::min<std::uint32_t>(tm.GetDrawGroupCount(), MAX_DRAW_GROUPS);
    for (std::uint32_t g = 0; g < group_count; ++g) {
        const auto group = static_cast<std::uint16_t>(g);
        const std::uint32_t base = g < region_base_.size() ? region_base_[g] : 0u;
        const std::uint32_t count = g < region_count_.size() ? region_count_[g] : 0u;
        if (count == 0) continue;

        const std::uint16_t tech_id = tm.GetDrawGroupTechnique(group);
        if (tech_id == std::numeric_limits<std::uint16_t>::max()) continue;
        auto* tech = tm.GetTechnique(tech_id);
        if (!tech) continue;

        const std::uint32_t variant = tm.GetDrawGroupVariant(group);
        const std::uint32_t render_state = tm.GetDrawGroupRenderState(group);
        const auto coverage = tech->GetDepthCoverage(variant);
        if (!GroupWritesPrepassDepth(tech->pipeline_flags.participates_in_depth_pass,
                                     render_state, coverage)) {
            continue;
        }

        const std::uint32_t draw_key = TechniqueManager::DrawKeyState::PackDrawKey(
            variant, render_state);
        if (!BindGroupDepthPipeline(cmd, *tech, draw_key, coverage, bindless_set,
                                    engine_sets, scene_uniform_set, camera_pos)) {
            LOGIFACE_LOG(error, std::format(
                "DepthPrepass: group {} depth pipeline unavailable, skipping", g));
            continue;
        }

        if (mid) {
            cmd.drawIndexedIndirectCount(
                *fr[BufIdx::depth_commands].GetBuffer(),
                static_cast<vk::DeviceSize>(base) * sizeof(vk::DrawIndexedIndirectCommand),
                *fr[BufIdx::depth_technique_counts].GetBuffer(),
                static_cast<vk::DeviceSize>(g) * sizeof(std::uint32_t),
                count, sizeof(vk::DrawIndexedIndirectCommand));
        } else {
            // CID: one draw per surviving submesh. Unused slots in the group's
            // region were zeroed by PrepareCompute (indexCount 0).
            for (std::uint32_t i = 0; i < count; ++i) {
                cmd.drawIndexedIndirect(
                    *fr[BufIdx::depth_commands].GetBuffer(),
                    static_cast<vk::DeviceSize>(base + i) *
                        sizeof(vk::DrawIndexedIndirectCommand),
                    1, sizeof(vk::DrawIndexedIndirectCommand));
            }
        }
    }
}

void SceneRenderer::OccluderPrepass(vk::CommandBuffer cmd, std::uint32_t w, std::uint32_t h,
                                    std::uint32_t fi,
                                    VulkanEngine::TechniqueManager::TechniqueManager& tm,
                                    VulkanEngine::BindlessManager::BindlessManager& bm,
                                    const glm::mat4& view) {
    auto& fr = frames_[fi % frames_in_flight_];
    if (!depth_slot_.Get()) {
        LOGIFACE_LOG(warn, "OccluderPrepass: depth pipeline is null, skipping");
        return;
    }
    if (!current_entity_count_) {
        LOGIFACE_LOG(trace, "OccluderPrepass: current_entity_count_ is 0, skipping");
        return;
    }
    LOGIFACE_LOG(trace, "OccluderPrepass: selected occluders (" + std::to_string(w) + "x" +
                 std::to_string(h) + ")");
    cmd.setViewport(0, VulkanEngine::PipelinePass::SceneViewport(w, h));
    cmd.setScissor(0, vk::Rect2D({0, 0}, {w, h}));

    const bool mid = draw_mode_ == DrawMode::MID;
    if (mid) {
        cmd.bindIndexBuffer(*fr[BufIdx::draw_indices].GetBuffer(), 0, vk::IndexType::eUint32);
    } else {
        cmd.bindIndexBuffer(*fr[BufIdx::occluder_indices].GetBuffer(), 0, vk::IndexType::eUint32);
    }

    const std::array<vk::DescriptorSet, 4> engine_sets{
        fr[SetIdx::empty].GetHandle(),
        fr[SetIdx::submesh_vertex].GetHandle(),
        fr[SetIdx::vertex_buffers].GetHandle(),
        fr[SetIdx::indirection].GetHandle()
    };
    const vk::DescriptorSet bindless_set = bm.GetDescriptorSet();
    const vk::DescriptorSet scene_uniform_set = GetSceneUniformSet();
    const glm::mat4 inv_view = glm::inverse(view);
    float camera_pos[4] = {inv_view[3][0], inv_view[3][1], inv_view[3][2], 0.0f};

    const std::uint32_t group_count =
        std::min<std::uint32_t>(tm.GetDrawGroupCount(), MAX_DRAW_GROUPS);
    for (std::uint32_t g = 0; g < group_count; ++g) {
        const auto group = static_cast<std::uint16_t>(g);
        const std::uint32_t base = g < region_base_.size() ? region_base_[g] : 0u;
        const std::uint32_t count = g < region_count_.size() ? region_count_[g] : 0u;
        if (count == 0) continue;

        const std::uint16_t tech_id = tm.GetDrawGroupTechnique(group);
        if (tech_id == std::numeric_limits<std::uint16_t>::max()) continue;
        auto* tech = tm.GetTechnique(tech_id);
        if (!tech) continue;

        const std::uint32_t variant = tm.GetDrawGroupVariant(group);
        const std::uint32_t render_state = tm.GetDrawGroupRenderState(group);
        const auto coverage = tech->GetDepthCoverage(variant);
        // Occluder selection already required the depth-pass bit, so an
        // excluded row cannot appear here; only the pipeline choice is left.
        const std::uint32_t draw_key = TechniqueManager::DrawKeyState::PackDrawKey(
            variant, render_state);
        if (!BindGroupDepthPipeline(cmd, *tech, draw_key, coverage, bindless_set,
                                    engine_sets, scene_uniform_set, camera_pos)) {
            LOGIFACE_LOG(error, std::format(
                "OccluderPrepass: group {} depth pipeline unavailable, skipping", g));
            continue;
        }

        if (mid) {
            cmd.drawIndexedIndirectCount(
                *fr[BufIdx::occluder_commands].GetBuffer(),
                static_cast<vk::DeviceSize>(base) * sizeof(vk::DrawIndexedIndirectCommand),
                *fr[BufIdx::occluder_technique_counts].GetBuffer(),
                static_cast<vk::DeviceSize>(g) * sizeof(std::uint32_t),
                count, sizeof(vk::DrawIndexedIndirectCommand));
        } else {
            for (std::uint32_t i = 0; i < count; ++i) {
                cmd.drawIndexedIndirect(
                    *fr[BufIdx::occluder_commands].GetBuffer(),
                    static_cast<vk::DeviceSize>(base + i) *
                        sizeof(vk::DrawIndexedIndirectCommand),
                    1, sizeof(vk::DrawIndexedIndirectCommand));
            }
        }
    }
}

void SceneRenderer::Render(vk::CommandBuffer cmd,
                            VulkanEngine::ComponentRegistry&,
                            VulkanEngine::TechniqueManager::TechniqueManager& tm,
                            VulkanEngine::BindlessManager::BindlessManager& bm,
                            const glm::mat4& proj, const glm::mat4& view,
                            std::uint32_t w, std::uint32_t h, std::uint32_t fi) {
    if (!w || !h) return;
    if (!current_entity_count_) {
        LOGIFACE_LOG(trace, "Render: current_entity_count_ is 0, skipping");
        return;
    }
    auto& fr = frames_[fi % frames_in_flight_];

    cmd.setViewport(0, VulkanEngine::PipelinePass::SceneViewport(w, h));
    cmd.setScissor(0, vk::Rect2D({0, 0}, {w, h}));

    // Engine descriptor set handles (used by both legacy and BaseTechnique paths)
    const vk::DescriptorSet engine_set0 = bm.GetDescriptorSet();
    const vk::DescriptorSet engine_set1 = fr[SetIdx::submesh_vertex].GetHandle();
    const vk::DescriptorSet engine_set2 = fr[SetIdx::vertex_buffers].GetHandle();
    const vk::DescriptorSet engine_set3 = fr[SetIdx::indirection].GetHandle();

    LOGIFACE_LOG(trace, "RenderMain: submesh_count=" + std::to_string(current_entity_count_) +
                 " techniques=" + std::to_string(tm.GetTechniqueCount()));

    // Scene uniform set (set 4) — lighting data (device-local storage)
    const vk::DescriptorSet scene_uniform_set = GetSceneUniformSet();

    // Camera position for fragment shader (push constant at offset 0)
    const glm::mat4 inv_view = glm::inverse(view);
    float camera_pos[4] = {inv_view[3][0], inv_view[3][1], inv_view[3][2], 0.0f};

    const bool mid = draw_mode_ == DrawMode::MID;
    cmd.bindIndexBuffer(
        mid ? *fr[BufIdx::draw_indices].GetBuffer() : *fr[BufIdx::main_indices].GetBuffer(),
        0, vk::IndexType::eUint32);

    // The collect arrays and command regions are indexed by draw group. A group
    // resolves to its owning technique, which owns the pipeline and the shared
    // layout/descriptor sets; render-state variants reuse the same set (variant
    // pipelines are added by the variant work, and the pipeline is selected per
    // group there).
    const std::uint32_t group_count =
        std::min<std::uint32_t>(tm.GetDrawGroupCount(), MAX_DRAW_GROUPS);
    for (std::uint32_t g = 0; g < group_count; ++g) {
        const std::uint16_t tech_id = tm.GetDrawGroupTechnique(static_cast<std::uint16_t>(g));
        if (tech_id == std::numeric_limits<std::uint16_t>::max()) continue;
        auto* tech = tm.GetTechnique(tech_id);
        if (!tech) continue;
        if (!tech->pipeline_flags.participates_in_collect) continue;

        // Materialize this group's pipeline variant on first use. The group
        // stores interface variant and render state separately; the composite
        // draw key rebuilds from them. A group with no matching variant (or the
        // base key 0) falls back to the base pipeline. The layout is shared
        // across variants, so the descriptor sets stay valid.
        const std::uint32_t draw_key = TechniqueManager::DrawKeyState::PackDrawKey(
            tm.GetDrawGroupVariant(g), tm.GetDrawGroupRenderState(g));
        if (!tech->HasVariant(draw_key)) {
            (void)tech->EnsureVariant(draw_key, *shader_mgr_, *pipeline_factory_);
        }
        auto pipeline = tech->GetPipeline(draw_key);
        auto layout = tech->GetPipelineLayout();
        if (!pipeline || !layout) {
            LOGIFACE_LOG(trace, std::format("RenderMain: group {} missing pipeline or layout, skipping", g));
            continue;
        }

        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        LOGIFACE_LOG(trace, std::format("RenderMain: group {} bound pipeline 0x{:x}",
                                        g, HandleToU64(pipeline)));

        std::array<vk::DescriptorSet, 16> ds{};
        std::uint32_t slot = 0;
        ds[slot++] = engine_set0;  // set 0: bindless textures
        ds[slot++] = engine_set1;  // set 1: submesh vertex data
        ds[slot++] = engine_set2;  // set 2: raw vertex buffers
        ds[slot++] = engine_set3;  // set 3: indirection
        ds[slot++] = scene_uniform_set;  // set 4: scene uniforms (lighting, camera)

        // Bind technique custom descriptor sets (BlockArray/Shared buffers) at slots 5+
        for (const auto& tech_ds : tech->GetCustomDescriptorSets()) {
            ds[slot++] = tech_ds;
        }

        cmd.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            layout,
            0,
            vk::ArrayProxy<const vk::DescriptorSet>(slot, ds.data()),
            {}
        );

        // Camera position (fragment-stage push constant at offset 0, 16 bytes)
        cmd.pushConstants(layout, vk::ShaderStageFlagBits::eFragment, 0, 16, camera_pos);

        if (mid) {
            const std::uint32_t base = (g < region_base_.size()) ? region_base_[g] : 0u;
            const std::uint32_t cnt = (g < region_count_.size()) ? region_count_[g] : 0u;
            if (cnt == 0) continue;
            cmd.drawIndexedIndirectCount(
                *fr[BufIdx::main_commands].GetBuffer(),
                static_cast<vk::DeviceSize>(base) * sizeof(vk::DrawIndexedIndirectCommand),
                *fr[BufIdx::technique_counts].GetBuffer(),
                static_cast<vk::DeviceSize>(g) * sizeof(std::uint32_t),
                cnt, sizeof(vk::DrawIndexedIndirectCommand));
        } else {
            const vk::DeviceSize draw_cmd_offset =
                static_cast<vk::DeviceSize>(g) * sizeof(vk::DrawIndexedIndirectCommand);
            cmd.drawIndexedIndirect(*fr[BufIdx::technique_draw_commands].GetBuffer(),
                                    draw_cmd_offset, 1,
                                    sizeof(vk::DrawIndexedIndirectCommand));
        }
    }
}

void SceneRenderer::DispatchExpand(vk::CommandBuffer cmd, std::uint32_t cnt,
                                    const glm::mat4& vp, std::uint32_t fi) {
    auto& fr = frames_[fi % frames_in_flight_];
    if (!cnt) {
        LOGIFACE_LOG(trace, "DispatchExpand: cnt is 0, skipping");
        return;
    }
    LOGIFACE_LOG(trace, "DispatchExpand: submeshes=" + std::to_string(cnt));
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, expand_slot_.Get());
    const std::array<vk::DescriptorSet, 2> ds{
        fr[SetIdx::expand].GetHandle(),
        fr[SetIdx::index_buffers].GetHandle()
    };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, Layout(SceneGpuLayout::PipelineLayoutId::expand),
                             0, ds, {});
    const ExpandPC pc{ .vp = vp, .cnt = cnt, .p0 = 0, .p1 = 0,
                 .hasActiveDeform = has_active_deform_ ? 1u : 0u,
                 .regionSpan = scene_capacity_.deform_vertex_span };
    cmd.pushConstants(Layout(SceneGpuLayout::PipelineLayoutId::expand), vk::ShaderStageFlagBits::eCompute,
                       0, sizeof(ExpandPC), &pc);
    // One workgroup per submesh: the whole group cooperates on the index copy.
    cmd.dispatch(cnt, 1, 1);
}

void SceneRenderer::DispatchDeform(vk::CommandBuffer cmd, const std::uint32_t fi) {
    auto& fr = frames_[fi % frames_in_flight_];
    const std::span<const ModifierDispatch> dispatches = GetModifierDispatches(fi);
    if (dispatches.empty() || !layouts_[SceneGpuLayout::PipelineLayoutId::deform]) {
        return;
    }
    LOGIFACE_LOG(trace, "DispatchDeform: stages=" + std::to_string(dispatches.size()));

    // Set 0 is the shared vertex-buffer table (both its bindings are visible to
    // compute); set 1 is the deform input set. Every modifier pipeline shares
    // this layout, so the sets stay bound across vkCmdBindPipeline.
    const std::array<vk::DescriptorSet, 2> sets{
        fr[SetIdx::vertex_buffers].GetHandle(),
        fr[SetIdx::deform].GetHandle(),
    };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, Layout(SceneGpuLayout::PipelineLayoutId::deform),
                           0, sets, {});

    for (std::size_t i = 0; i < dispatches.size(); ++i) {
        const ModifierDispatch& dispatch = dispatches[i];
        const auto it = modifier_pipelines_.find(ModifierKey(dispatch.modifier));
        if (it == modifier_pipelines_.end() || !it->second.ready || it->second.dead) {
            // Gather already dropped non-ready modifiers; a pipeline retired
            // between gather and submit leaves its stage unwritten. Skipping it
            // is the safe outcome: the vertex shaders read whatever region the
            // remaining stages produced.
            continue;
        }
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, it->second.slot.Get());
        const ModifierPC pc{
            .submesh_index = dispatch.submesh_index,
            .stage = dispatch.stage,
            .src_slot = dispatch.src_slot,
            .src_base = dispatch.src_base,
            .vertex_count = dispatch.vertex_count,
            .param_offset = dispatch.param_offset,
            .param_size = dispatch.param_size,
            .region_span = scene_capacity_.deform_vertex_span,
        };
        cmd.pushConstants(Layout(SceneGpuLayout::PipelineLayoutId::deform), vk::ShaderStageFlagBits::eCompute,
                          0, sizeof(ModifierPC), &pc);
        const std::uint32_t group = std::max(dispatch.workgroup_size, 1u);
        cmd.dispatch((dispatch.vertex_count + group - 1) / group, 1, 1);

        // Stage k+1 reads stage k's output. The render graph inserts a barrier
        // between passes, but not between the dispatches inside this one, so
        // each dispatch's writes must be visible to the next.
        if (i + 1 < dispatches.size()) {
            vk::MemoryBarrier barrier{};
            barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
            barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead |
                                    vk::AccessFlagBits::eShaderWrite;
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                vk::PipelineStageFlagBits::eComputeShader,
                                {}, barrier, {}, {});
        }
    }
}

void SceneRenderer::DispatchHiZGen(vk::CommandBuffer cmd, std::uint32_t width, std::uint32_t height,
                                    std::uint32_t frame_index, std::uint32_t) {
    auto& frame_resource = frames_[frame_index % frames_in_flight_];
    if (!current_entity_count_) {
        LOGIFACE_LOG(trace, "DispatchHiZGen: current_entity_count_ is 0, skipping");
        return;
    }
    if (!frame_resource[SetIdx::hiz].IsValid()) {
        LOGIFACE_LOG(trace, "DispatchHiZGen: hiz_set invalid, skipping");
        return;
    }
    if (width != depth_width_ || height != depth_height_) {
        LOGIFACE_LOG(warn, "DispatchHiZGen: dimensions changed (" + std::to_string(width) + "x" +
                     std::to_string(height) + " != " + std::to_string(depth_width_) + "x" +
                     std::to_string(depth_height_) + "), skipping");
        return;
    }

    LOGIFACE_LOG(trace, "DispatchHiZGen: " + std::to_string(width) + "x" + std::to_string(height) +
                 " mips=" + std::to_string(hiz_mip_count_));

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, hiz_slot_.Get());
    const std::array<vk::DescriptorSet, 1> ds { frame_resource[SetIdx::hiz].GetHandle() };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, Layout(SceneGpuLayout::PipelineLayoutId::hiz), 0, ds, {});

    // Mirrors hiz_gen.slang's numthreads(16, 16, 1).
    constexpr std::uint32_t kHiZGroupThreads = 16;

    // Every level the cull shaders can sample must be produced here.
    // Iterating to hiz_mip_count_ (not hiz_mip_count_ - 1) lets the final iteration run with bc == 1, whose second reduction the shader already guards out.
    for (std::uint32_t bl = 0; bl < hiz_mip_count_; bl += HIZ_BATCH) {
        const std::uint32_t bc = std::min(HIZ_BATCH, hiz_mip_count_ - bl);
        const std::uint32_t sw = width >> bl;
        const std::uint32_t sh = height >> bl;
        HiZPC pc{ bl, sw, sh, bc };
        cmd.pushConstants(Layout(SceneGpuLayout::PipelineLayoutId::hiz), vk::ShaderStageFlagBits::eCompute, 0, sizeof(HiZPC), &pc);

        // Each thread reduces one HIZ_TILE_STRIDE x HIZ_TILE_STRIDE tile of the source level.
        // The shader's bound guards handle a partial trailing tile.
        const std::uint32_t tiles_x = (sw + HIZ_TILE_STRIDE - 1) / HIZ_TILE_STRIDE;
        const std::uint32_t tiles_y = (sh + HIZ_TILE_STRIDE - 1) / HIZ_TILE_STRIDE;
        cmd.dispatch((tiles_x + kHiZGroupThreads - 1) / kHiZGroupThreads,
                     (tiles_y + kHiZGroupThreads - 1) / kHiZGroupThreads, 1);

        // The next iteration reads level bl + 1, which this dispatch just wrote, so order the two.
        // The last dispatch needs no barrier: the render graph orders its write against the pass that samples the pyramid next.
        if (bl + bc < hiz_mip_count_) {
            vk::MemoryBarrier hiz_mb{};

            hiz_mb.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
            hiz_mb.dstAccessMask = vk::AccessFlagBits::eShaderRead;

            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                vk::PipelineStageFlagBits::eComputeShader,
                                {}, hiz_mb, {}, {});
        }
    }
}

void SceneRenderer::DispatchOcclusion(vk::CommandBuffer cmd, std::uint32_t fi) {
    auto& fr = frames_[fi % frames_in_flight_];
    if (!current_entity_count_) {
        LOGIFACE_LOG(trace, "DispatchOcclusion: current_entity_count_ is 0, skipping");
        return;
    }
    if (!fr[SetIdx::occlusion].IsValid()) {
        LOGIFACE_LOG(trace, "DispatchOcclusion: occlusion_set invalid, skipping");
        return;
    }
    LOGIFACE_LOG(trace, "DispatchOcclusion: submeshes=" + std::to_string(current_entity_count_));

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, occlusion_slot_.Get());
    const std::array<vk::DescriptorSet, 1> ds{ fr[SetIdx::occlusion].GetHandle() };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, Layout(SceneGpuLayout::PipelineLayoutId::occlusion),
                             0, ds, {});
    const std::uint32_t hiz_w = (depth_width_ + 1) / 2;
    const std::uint32_t hiz_h = (depth_height_ + 1) / 2;
    OccPC pc{ current_entity_count_, 1, hiz_w, hiz_h, fr.proj_info };
    cmd.pushConstants(Layout(SceneGpuLayout::PipelineLayoutId::occlusion), vk::ShaderStageFlagBits::eCompute,
                       0, sizeof(OccPC), &pc);
    cmd.dispatch((current_entity_count_ + 63) / 64, 1, 1);
}

void SceneRenderer::DispatchCollect(vk::CommandBuffer cmd, std::uint32_t fi) {
    auto& fr = frames_[fi % frames_in_flight_];
    if (!current_entity_count_) {
        LOGIFACE_LOG(trace, "DispatchCollect: current_entity_count_ is 0, skipping");
        return;
    }
    // Only keys below the live draw-key count can appear in cull entries; the
    // GPU tables are larger, but scanning the capacity would waste a serial
    // prefix pass and the collect-write loop. The count is a hard upper bound
    // (set from the registered technique count and the region table).
    const std::uint32_t key_count =
        std::min(technique_count_, MAX_DRAW_GROUPS);
    LOGIFACE_LOG(trace, "DispatchCollect: submeshes=" + std::to_string(current_entity_count_) +
                 " drawKeys=" + std::to_string(key_count));
    const bool mid = draw_mode_ == DrawMode::MID;

    const auto barrier = [&cmd]() {
        vk::MemoryBarrier mb{};
        mb.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        mb.dstAccessMask = vk::AccessFlagBits::eShaderRead |
                           vk::AccessFlagBits::eShaderWrite;
        cmd.pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eComputeShader,
            {}, mb, {}, {});
    };

    const std::array<vk::DescriptorSet, 1> ds1{ fr[SetIdx::collect].GetHandle() };

    const std::uint32_t groups = (current_entity_count_ + 255) / 256;
    if (!mid) {
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, collect_count_slot_.Get());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, Layout(SceneGpuLayout::PipelineLayoutId::collect_count),
                                0, ds1, {});

        // CID: count -> global prefix -> compact -> command emission.
        CollectPC pc0{ current_entity_count_, 0, key_count, 0 };
        cmd.pushConstants(Layout(SceneGpuLayout::PipelineLayoutId::collect_count), vk::ShaderStageFlagBits::eCompute,
                          0, sizeof(CollectPC), &pc0);
        cmd.dispatch(groups, 1, 1);

        barrier();

        CollectPC pc1{ current_entity_count_, 0, key_count, 1 };
        cmd.pushConstants(Layout(SceneGpuLayout::PipelineLayoutId::collect_count), vk::ShaderStageFlagBits::eCompute,
                          0, sizeof(CollectPC), &pc1);
        cmd.dispatch(1, 1, 1);

        barrier();

        CollectPC pc2{ current_entity_count_, 0, key_count, 2 };
        cmd.pushConstants(Layout(SceneGpuLayout::PipelineLayoutId::collect_count), vk::ShaderStageFlagBits::eCompute,
                          0, sizeof(CollectPC), &pc2);
        // One workgroup per submesh so the index copy is parallel.
        cmd.dispatch(current_entity_count_, 1, 1);

        barrier();

        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, collect_write_slot_.Get());
        const std::array<vk::DescriptorSet, 1> ds2{ fr[SetIdx::collect_write].GetHandle() };
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, Layout(SceneGpuLayout::PipelineLayoutId::collect_write),
                                0, ds2, {});
        WritePC pcw{ current_entity_count_, 0, key_count, 0 };
        cmd.pushConstants(Layout(SceneGpuLayout::PipelineLayoutId::collect_write), vk::ShaderStageFlagBits::eCompute,
                          0, sizeof(WritePC), &pcw);
        cmd.dispatch(1, 1, 1);
    } else {
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, collect_count_mid_slot_.Get());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, Layout(SceneGpuLayout::PipelineLayoutId::collect_count),
                                0, ds1, {});

        // MID: one flat compact pass appends commands into CPU-prefixed regions
        // and publishes the per-technique alive count. No epilogue dispatch.
        CollectPC pcMid{ current_entity_count_, 0, key_count, 2 };
        cmd.pushConstants(Layout(SceneGpuLayout::PipelineLayoutId::collect_count), vk::ShaderStageFlagBits::eCompute,
                          0, sizeof(CollectPC), &pcMid);
        cmd.dispatch((current_entity_count_ + 63) / 64, 1, 1);
    }
}

void SceneRenderer::DispatchOccluderSelect(vk::CommandBuffer cmd,
                                            std::uint32_t w,
                                            std::uint32_t h,
                                            std::uint32_t fi) {
    // §5.1-A: OBB screen-area threshold selection + occluder indirection
    // compaction into the occluder-prepass draw command. Any set of large
    // real-geometry occluders is valid — overflow only misses culls.
    if (!current_entity_count_) {
        LOGIFACE_LOG(trace, "DispatchOccluderSelect: current_entity_count_ is 0, skipping");
        return;
    }
    auto& fr = frames_[fi % frames_in_flight_];
    const bool mid = draw_mode_ == DrawMode::MID;
    auto& slot = mid ? occluder_select_mid_slot_ : occluder_select_slot_;
    if (!slot.Get()) {
        LOGIFACE_LOG(warn, "DispatchOccluderSelect: pipeline is null, skipping");
        return;
    }
    LOGIFACE_LOG(trace, "DispatchOccluderSelect: submeshes=" + std::to_string(current_entity_count_));

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, slot.Get());
    const std::array<vk::DescriptorSet, 1> ds{ fr[SetIdx::occluder_select].GetHandle() };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, Layout(SceneGpuLayout::PipelineLayoutId::occluder_select),
                             0, ds, {});
    const float screenArea = static_cast<float>(w) * static_cast<float>(h);
    OccluderSelectPC pc{
        current_entity_count_,
        static_cast<std::uint32_t>(screenArea * kOccluderMinAreaFraction),
        w, h };
    cmd.pushConstants(Layout(SceneGpuLayout::PipelineLayoutId::occluder_select), vk::ShaderStageFlagBits::eCompute,
                       0, sizeof(OccluderSelectPC), &pc);
    // MID: one thread per submesh. CID: one workgroup per submesh for the copy.
    cmd.dispatch(mid ? (current_entity_count_ + 63) / 64 : current_entity_count_, 1, 1);
}

void SceneRenderer::DispatchPreCull(vk::CommandBuffer cmd, std::uint32_t fi) {
    // §5.1-D / §5.3: frustum reject + Hi-Z test against the occluder-only
    // pyramid, then compact survivors into the depth-prepass indirection.
    // Culled submeshes get indexCount = 0 so the post-prepass occlusion pass
    // and collect skip them for free.
    if (!current_entity_count_) {
        LOGIFACE_LOG(trace, "DispatchPreCull: current_entity_count_ is 0, skipping");
        return;
    }
    auto& fr = frames_[fi % frames_in_flight_];
    const bool mid = draw_mode_ == DrawMode::MID;
    auto& slot = mid ? pre_cull_mid_slot_ : pre_cull_slot_;
    LOGIFACE_LOG(trace, "DispatchPreCull: submeshes=" + std::to_string(current_entity_count_));

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, slot.Get());
    const std::array<vk::DescriptorSet, 1> ds{ fr[SetIdx::occlusion].GetHandle() };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, Layout(SceneGpuLayout::PipelineLayoutId::pre_cull),
                             0, ds, {});
    const std::uint32_t hiz_w = (depth_width_ + 1) / 2;
    const std::uint32_t hiz_h = (depth_height_ + 1) / 2;
    PreCullPC pc{ current_entity_count_, hiz_w, hiz_h, 0, fr.proj_info };
    cmd.pushConstants(Layout(SceneGpuLayout::PipelineLayoutId::pre_cull), vk::ShaderStageFlagBits::eCompute,
                       0, sizeof(PreCullPC), &pc);
    // MID: one thread per submesh. CID: one workgroup per submesh for the copy.
    cmd.dispatch(mid ? (current_entity_count_ + 63) / 64 : current_entity_count_, 1, 1);
}

void SceneRenderer::InitializeHizFirstFrame(vk::CommandBuffer cmd) {
    if (hiz_initialized_) return;
    for (std::uint32_t i = 0; i < frames_in_flight_; ++i) {
        auto& f = frames_[i];
        const vk::ImageSubresourceRange hiz_range(
            vk::ImageAspectFlagBits::eColor, 0, hiz_mip_count_, 0, 1);
        const vk::ImageMemoryBarrier imb(
            vk::AccessFlagBits::eNone, vk::AccessFlagBits::eTransferWrite,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
            *f.hiz_image, hiz_range);
        cmd.pipelineBarrier(
            vk::PipelineStageFlagBits::eTopOfPipe,
            vk::PipelineStageFlagBits::eTransfer,
            {}, {}, {}, imb);
        const vk::ClearColorValue clear(1.0f, 1.0f, 1.0f, 1.0f);
        cmd.clearColorImage(*f.hiz_image, vk::ImageLayout::eGeneral,
                            clear, hiz_range);
        const vk::ImageMemoryBarrier post_clear(
            vk::AccessFlagBits::eTransferWrite,
            vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
            vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral,
            vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
            *f.hiz_image, hiz_range);
        cmd.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eComputeShader,
            {}, {}, {}, post_clear);
    }
    hiz_initialized_ = true;
}

} // namespace VulkanEngine::SceneRenderer
