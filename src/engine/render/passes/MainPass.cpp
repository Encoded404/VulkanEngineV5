module;

#include <cstdint>
#include <logging/logging_macros.hpp>

module VulkanEngine.Render.Passes.MainPass;

import std;
import logiface;
import vulkan_hpp;
import VulkanEngine.TechniqueManager;
import VulkanEngine.BindlessManager;
import VulkanEngine.PipelinePass;

namespace VulkanEngine::SceneRenderer {

MainPass::MainPass(SceneRenderer& sr) : scene_renderer_(sr) {}

void MainPass::Execute(vk::CommandBuffer cmd,
                        VulkanEngine::TechniqueManager::TechniqueManager& tm,
                        VulkanEngine::BindlessManager::BindlessManager& bm,
                        vk::DescriptorSet submesh_vertex_set,
                        vk::DescriptorSet bindless_vertex_set,
                        vk::DescriptorSet indirection_raw_set,
                        vk::Buffer technique_draw_commands_buffer,
                        std::uint32_t w, std::uint32_t h,
                        std::uint32_t entity_count) {
    if (!w || !h) return;
    if (!entity_count) {
        LOGIFACE_LOG(debug, "MainPass::Execute: entity_count is 0, skipping");
        return;
    }

    cmd.setViewport(0, vk::Viewport(0, static_cast<float>(h), static_cast<float>(w),
                                     -static_cast<float>(h), 0, 1));
    cmd.setScissor(0, vk::Rect2D({0, 0}, {w, h}));

    const vk::DescriptorSet engine_set0 = bm.GetDescriptorSet();
    const vk::DescriptorSet engine_set1 = submesh_vertex_set;
    const vk::DescriptorSet engine_set2 = bindless_vertex_set;
    const vk::DescriptorSet engine_set3 = indirection_raw_set;

    LOGIFACE_LOG(trace, "MainPass::Execute: submesh_count=" + std::to_string(entity_count) +
                 " techniques=" + std::to_string(tm.GetTechniqueCount()));

    for (uint16_t t = 0; t < tm.GetTechniqueCount(); ++t) {
        auto* tech = tm.GetTechnique(t);
        if (!tech) continue;
        auto pipeline = tech->GetPipeline();
        auto layout = tech->GetPipelineLayout();
        if (!pipeline || !layout) continue;

        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

        std::array<vk::DescriptorSet, 16> ds{};
        std::uint32_t slot = 0;
        ds[slot++] = engine_set0;
        ds[slot++] = engine_set1;
        ds[slot++] = engine_set2;
        ds[slot++] = engine_set3;

        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0,
                               vk::ArrayProxy<const vk::DescriptorSet>(slot, ds.data()), {});

        const vk::DeviceSize draw_cmd_offset =
            static_cast<vk::DeviceSize>(t) * sizeof(vk::DrawIndirectCommand);
        cmd.drawIndirect(technique_draw_commands_buffer, draw_cmd_offset, 1,
                          sizeof(vk::DrawIndirectCommand));
    }
}

void MainPass::Setup(VulkanEngine::PipelinePass::PassSetupContext& /*ctx*/) {}

void MainPass::Execute(const VulkanEngine::PipelinePass::FrameContext& /*ctx*/,
                        vk::CommandBuffer /*cmd*/) {}

} // namespace VulkanEngine::SceneRenderer
