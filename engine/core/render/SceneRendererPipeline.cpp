module;

#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)

#include <logging/logging_macros.hpp>

module VulkanEngine.SceneRenderer;

import std;
import std.compat;

import logiface;

import vulkan;

import VulkanBackend.Vulkan.VulkanDebugUtils;
import VulkanEngine.PipelineCompiler;
import VulkanEngine.PipelineFactory;
import VulkanEngine.PipelineTemplates;
import VulkanEngine.SceneGpuLayout;

namespace VulkanEngine::SceneRenderer {

// The built-in compute passes, in creation order.
// Every field is a member pointer or a table id, so creation (CreateScenePipelines), hot-reload polling (PollShaders) and the draw-mode rebuild (RebuildCompactionPipelines) all read this one list.
// The depth prepass is the renderer's only graphics pipeline and stays next to its template below.
std::span<const SceneRenderer::ComputePipelineRow> SceneRenderer::ComputePipelineRows() noexcept {
    using LayoutId = SceneGpuLayout::PipelineLayoutId;
    static constexpr std::array kRows{
        ComputePipelineRow{.name = "expand", .shader = &EngineShaderIds::expand_comp,
                           .layout = LayoutId::expand, .slot = &SceneRenderer::expand_slot_,
                           .desc = &SceneRenderer::expand_desc_, .draw_mode_specialized = true},
        ComputePipelineRow{.name = "occlusion", .shader = &EngineShaderIds::occlusion_cull_comp,
                           .layout = LayoutId::occlusion,
                           .slot = &SceneRenderer::occlusion_slot_,
                           .desc = &SceneRenderer::occlusion_desc_,
                           .draw_mode_specialized = false},
        ComputePipelineRow{.name = "occluder-select",
                           .shader = &EngineShaderIds::occluder_select_comp,
                           .layout = LayoutId::occluder_select,
                           .slot = &SceneRenderer::occluder_select_slot_,
                           .desc = &SceneRenderer::occluder_select_desc_,
                           .draw_mode_specialized = true},
        // The MID variants are rows of their own: one thread per submesh, no group copy or barrier, and no draw-mode specialization constant to rebuild.
        ComputePipelineRow{.name = "occluder-select-mid",
                           .shader = &EngineShaderIds::occluder_select_comp_mid,
                           .layout = LayoutId::occluder_select,
                           .slot = &SceneRenderer::occluder_select_mid_slot_,
                           .desc = &SceneRenderer::occluder_select_mid_desc_,
                           .draw_mode_specialized = false},
        ComputePipelineRow{.name = "pre-cull", .shader = &EngineShaderIds::pre_cull_comp,
                           .layout = LayoutId::pre_cull, .slot = &SceneRenderer::pre_cull_slot_,
                           .desc = &SceneRenderer::pre_cull_desc_, .draw_mode_specialized = true},
        ComputePipelineRow{.name = "pre-cull-mid", .shader = &EngineShaderIds::pre_cull_comp_mid,
                           .layout = LayoutId::pre_cull,
                           .slot = &SceneRenderer::pre_cull_mid_slot_,
                           .desc = &SceneRenderer::pre_cull_mid_desc_,
                           .draw_mode_specialized = false},
        ComputePipelineRow{.name = "collect-count",
                           .shader = &EngineShaderIds::collect_count_compact_comp,
                           .layout = LayoutId::collect_count,
                           .slot = &SceneRenderer::collect_count_slot_,
                           .desc = &SceneRenderer::collect_count_desc_,
                           .draw_mode_specialized = true},
        ComputePipelineRow{.name = "collect-count-mid",
                           .shader = &EngineShaderIds::collect_count_compact_comp_mid,
                           .layout = LayoutId::collect_count,
                           .slot = &SceneRenderer::collect_count_mid_slot_,
                           .desc = &SceneRenderer::collect_count_mid_desc_,
                           .draw_mode_specialized = false},
        // collect-write runs only in CID, reads no kCompactionMode constant, and shares no layout with the count pass.
        ComputePipelineRow{.name = "collect-write",
                           .shader = &EngineShaderIds::collect_write_comp,
                           .layout = LayoutId::collect_write,
                           .slot = &SceneRenderer::collect_write_slot_,
                           .desc = &SceneRenderer::collect_write_desc_,
                           .draw_mode_specialized = false},
        ComputePipelineRow{.name = "hiz-gen", .shader = &EngineShaderIds::hiz_gen_comp,
                           .layout = LayoutId::hiz, .slot = &SceneRenderer::hiz_slot_,
                           .desc = &SceneRenderer::hiz_desc_, .draw_mode_specialized = false},
    };
    return kRows;
}

    namespace {
        struct ExpandPC {
            glm::mat4 vp;
            std::uint32_t cnt;
            std::uint32_t p0;
            std::uint32_t p1;
            std::uint32_t hasActiveDeform;
            std::uint32_t regionSpan;
        };
        static_assert(sizeof(ExpandPC) == 84, "ExpandPC must match expand.slang");
        // projInfo = (|proj[0][0]|, |proj[1][1]|, proj[2][2], 1 if perspective else 0)
        struct OccPC { std::uint32_t cnt; std::uint32_t refineLevel; std::uint32_t hizWidth; std::uint32_t hizHeight; glm::vec4 projInfo; };
        struct HiZPC { std::uint32_t bl; std::uint32_t sw; std::uint32_t sh; std::uint32_t tc; };
        struct CollectPC { std::uint32_t cnt; std::uint32_t p0; std::uint32_t mt; std::uint32_t pass; };
        struct WritePC { std::uint32_t cnt; std::uint32_t p0; std::uint32_t techniqueCount; std::uint32_t p1; };
        struct OccluderSelectPC { std::uint32_t cnt; std::uint32_t minAreaPx; std::uint32_t screenW; std::uint32_t screenH; };
        struct PreCullPC { std::uint32_t cnt; std::uint32_t hizW; std::uint32_t hizH; std::uint32_t p0; glm::vec4 projInfo; };

        constexpr std::uint32_t PushConstantBytes(const SceneGpuLayout::PipelineLayoutId id) {
            return SceneGpuLayout::LayoutOf(id).push_constant_bytes;
        }

        // The layout rows own the push-constant sizes; these pin each row to the struct the recording code pushes.
        static_assert(sizeof(ExpandPC) == PushConstantBytes(SceneGpuLayout::PipelineLayoutId::expand),
                      "the expand layout row must declare sizeof(ExpandPC)");
        static_assert(sizeof(OccPC) == PushConstantBytes(SceneGpuLayout::PipelineLayoutId::occlusion),
                      "the occlusion layout row must declare sizeof(OccPC)");
        static_assert(sizeof(OccluderSelectPC) == PushConstantBytes(SceneGpuLayout::PipelineLayoutId::occluder_select),
                      "the occluder-select layout row must declare sizeof(OccluderSelectPC)");
        static_assert(sizeof(PreCullPC) == PushConstantBytes(SceneGpuLayout::PipelineLayoutId::pre_cull),
                      "the pre-cull layout row must declare sizeof(PreCullPC)");
        static_assert(sizeof(CollectPC) == PushConstantBytes(SceneGpuLayout::PipelineLayoutId::collect_count),
                      "the collect layout row must declare sizeof(CollectPC)");
        static_assert(sizeof(WritePC) == PushConstantBytes(SceneGpuLayout::PipelineLayoutId::collect_write),
                      "the collect-write layout row must declare sizeof(WritePC)");
        static_assert(sizeof(HiZPC) == PushConstantBytes(SceneGpuLayout::PipelineLayoutId::hiz),
                      "the hiz layout row must declare sizeof(HiZPC)");

        // One pipeline-layout creation shape for every scene pipeline: borrowed set layouts plus an optional compute push-constant range.
        std::unique_ptr<vk::raii::PipelineLayout> MakePipelineLayout(
                const VulkanBackend::Vulkan::IVulkanBootstrap& be,
                std::string_view layout_name,
                std::span<const vk::DescriptorSetLayout> set_layouts,
                std::uint32_t push_constant_size = 0) {
            const auto& dev = be.GetDevice();
            vk::PipelineLayoutCreateInfo li{};
            li.setLayoutCount = static_cast<std::uint32_t>(set_layouts.size());
            li.pSetLayouts = set_layouts.data();
            vk::PushConstantRange pr{};
            if (push_constant_size > 0) {
                pr.stageFlags = vk::ShaderStageFlagBits::eCompute;
                pr.size = push_constant_size;
                li.pushConstantRangeCount = 1;
                li.pPushConstantRanges = &pr;
            }
            auto layout = std::make_unique<vk::raii::PipelineLayout>(dev, li);
            VulkanBackend::Vulkan::SetVulkanObjectName(dev, *layout, std::string(layout_name));
            return layout;
        }
    } // anonymous namespace

bool SceneRenderer::CreateScenePipelines(VulkanBackend::Vulkan::IVulkanBootstrap& be,
                                          ShaderSystem::ShaderManager& shader_mgr,
                                          ShaderSystem::PipelineFactory& pipeline_factory,
                                          const EngineShaderIds& shader_ids) {
    LOGIFACE_LOG(debug, "Creating scene pipelines...");

    // The descriptor-set layouts and pools are created from SceneGpuLayout::kSceneSets before this runs; the pipeline layouts below only borrow the handles.

    // ── Pipeline layouts ──
    //
    // One loop over SceneGpuLayout::kPipelineLayouts: a row names the layout, its set order and its push-constant range.
    // The set order is the only place a pipeline's set index lives, so a set can be bound at a different index without touching the set row.
    for (const SceneGpuLayout::PipelineLayoutSpec& spec : SceneGpuLayout::kPipelineLayouts) {
        std::vector<vk::DescriptorSetLayout> set_layouts;
        set_layouts.reserve(spec.sets.size());
        for (const SceneGpuLayout::SetId set : spec.sets) {
            set_layouts.push_back(GetSetLayout(set));
        }
        LayoutSlot(spec.id) = MakePipelineLayout(be, spec.name, set_layouts,
                                                 spec.push_constant_bytes);
    }

    bool ok = true;

    // ── Depth-prepass graphics pipeline desc ──
    // Depth-only pass: no color attachments, so the blend state must not
    // declare any (the empty blend_attachments template yields that). The
    // vertex stage is draw-mode-specialized like the compute cullers.
    ShaderSystem::GraphicsPipelineTemplate depth_tmpl{
        .debug_name = "depth-prepass",
        .input_assembly = vk::PipelineInputAssemblyStateCreateInfo(
            {}, vk::PrimitiveTopology::eTriangleList),
        .rasterization = vk::PipelineRasterizationStateCreateInfo(
            {}, false, false, vk::PolygonMode::eFill, vk::CullModeFlagBits::eFront,
            vk::FrontFace::eClockwise, false, 0, 0, 0, 1.0f),
        .multisample = vk::PipelineMultisampleStateCreateInfo(
            {}, vk::SampleCountFlagBits::e1),
        .depth_stencil = vk::PipelineDepthStencilStateCreateInfo(
            {}, true, true, vk::CompareOp::eLess),
    };
    auto depth_desc = ShaderSystem::MakeGraphicsDesc(
        depth_tmpl, shader_ids.depth_indir_vert, shader_ids.depth_prepass_frag,
        Layout(SceneGpuLayout::PipelineLayoutId::depth), {}, be.GetDepthFormat());
    ApplyDrawModeSpec(depth_desc, ShaderSystem::DrawModeSpecValue(draw_mode_));
    depth_desc_ = depth_desc;

    // ── Compile the whole window in one submission ──
    // One batched driver call for every compute row plus the depth pipeline (per-path batching inside the factory: compute is always one call; graphics batching applies on the monolithic path).
    // Every delivery that fails records the error and fails initialization; the previous per-helper code swallowed these.
    ShaderSystem::PipelineFactoryExecutor executor{pipeline_factory};
    ShaderSystem::PipelineCompiler compiler{executor};
    for (const ComputePipelineRow& row : ComputePipelineRows()) {
        auto desc = ShaderSystem::MakeComputeDesc(
            ShaderSystem::ComputePipelineTemplate{.debug_name = row.name},
            shader_ids.*row.shader, Layout(row.layout));
        if (row.draw_mode_specialized) {
            ApplyDrawModeSpec(desc, ShaderSystem::DrawModeSpecValue(draw_mode_));
        }
        this->*row.desc = desc;
        ShaderSystem::PipelineSlot& slot = this->*row.slot;
        const std::string_view name = row.name;
        compiler.SubmitCompute(std::move(desc), name,
            [this, &slot, &ok, name](ShaderSystem::PipelineOutcome&& outcome) {
                if (!outcome.has_value()) {
                    LOGIFACE_LOG(error, std::format(
                        "SceneRenderer: {} pipeline creation failed: {}",
                        name, outcome.error().message));
                    ok = false;
                    return;
                }
                slot.Swap(std::move(*outcome), 0);
                VulkanBackend::Vulkan::SetVulkanObjectName(
                    backend_->GetDevice(), slot.Get(), std::format("{}-pipeline", name));
            });
    }
    {
        auto& slot = depth_slot_;
        compiler.SubmitGraphics(std::move(depth_desc), "depth-prepass",
            [this, &slot, &ok](ShaderSystem::PipelineOutcome&& outcome) {
                if (!outcome.has_value()) {
                    LOGIFACE_LOG(error, std::format(
                        "SceneRenderer: depth-prepass pipeline creation failed: {}",
                        outcome.error().message));
                    ok = false;
                    return;
                }
                slot.Swap(std::move(*outcome), 0);
                VulkanBackend::Vulkan::SetVulkanObjectName(
                    backend_->GetDevice(), slot.Get(), "depth-prepass-pipeline");
            });
    }
    compiler.Flush(shader_mgr);

    LOGIFACE_LOG(debug, ok ? "Scene pipelines created"
                           : "Scene pipeline creation failed");
    return ok;
}

bool SceneRenderer::RebuildCompactionPipelines() {
    if (!backend_ || !shader_mgr_ || !pipeline_factory_) return false;
    // Rebuild only the pipeline objects: the pipeline layouts are reused so pipelines still in a retire ring keep valid layouts.
    // The draw-mode spec value is updated on each stored desc first.
    //
    // The set to rebuild is the table's own `draw_mode_specialized` flag, so a pass that starts reading kCompactionMode only has to set that flag.
    // Technique pipelines also carry the draw-mode spec, but TechniqueManager owns them; the caller of Reinitialize re-specializes those (GameEngine::SetDrawMode).
    const auto rebuild = [this](ShaderSystem::PipelineSlot& slot, auto& desc) {
        if (!desc) return true;
        using Desc = std::remove_cvref_t<decltype(*desc)>;
        std::expected<ShaderSystem::PipelineProduct, ShaderSystem::PipelineError> result =
            std::unexpected(ShaderSystem::PipelineError{});
        if constexpr (std::same_as<Desc, ShaderSystem::ComputePipelineDesc>) {
            result = pipeline_factory_->CreateCompute(*desc, *shader_mgr_);
        } else {
            result = pipeline_factory_->CreateGraphics(*desc, *shader_mgr_);
        }
        if (!result.has_value()) {
            LOGIFACE_LOG(error, "SceneRenderer: failed to rebuild mode-specialized pipeline");
            return false;
        }
        slot.Swap(std::move(result.value()), 0);
        return true;
    };

    bool ok = true;
    for (const ComputePipelineRow& row : ComputePipelineRows()) {
        if (!row.draw_mode_specialized) continue;
        auto& desc = this->*row.desc;
        if (!desc) continue;
        ApplyDrawModeSpec(*desc, ShaderSystem::DrawModeSpecValue(draw_mode_));
        ok = rebuild(this->*row.slot, desc) && ok;
    }
    if (depth_desc_) {
        ApplyDrawModeSpec(*depth_desc_, ShaderSystem::DrawModeSpecValue(draw_mode_));
        ok = rebuild(depth_slot_, depth_desc_) && ok;
    }
    return ok;
}

} // namespace VulkanEngine::SceneRenderer