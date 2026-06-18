module;

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/matrix_transform.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/quaternion.hpp> //NOLINT(misc-include-cleaner)

#include <logging/logging_macros.hpp>

module VulkanEngine.SceneRenderer;

import std;
import std.compat;

import logiface;

import vulkan_hpp;

import VulkanBackend.Component;
import VulkanEngine.Components.Transform;
import VulkanEngine.Components.MeshReference;
import VulkanEngine.GpuResources;
import VulkanEngine.StandardMeshPipeline;
import VulkanEngine.TechniqueManager;
import VulkanEngine.BindlessManager;
import VulkanEngine.MaterialManager;

namespace VulkanEngine::SceneRenderer {
    namespace {
        struct ExpandPC { glm::mat4 vp; std::uint32_t cnt; std::uint32_t p0; std::uint32_t p1; };
        struct HiZPC { std::uint32_t bl; std::uint32_t sw; std::uint32_t sh; std::uint32_t tc; };
        struct OccPC { std::uint32_t cnt; std::uint32_t refineLevel; std::uint32_t hizWidth; std::uint32_t hizHeight; };
        struct CollectPC { std::uint32_t cnt; std::uint32_t p0; std::uint32_t mt; std::uint32_t pass; };
        struct WritePC { std::uint32_t cnt; std::uint32_t p0; std::uint32_t techniqueCount; std::uint32_t p1; };
        static constexpr std::uint32_t HIZ_BATCH = 2;

        static void WriteBlocks(vk::DescriptorSet ds, std::uint32_t binding,
                                GpuResources::BlockArray& buf,
                                vk::DescriptorType desc_type,
                                const vk::raii::Device& dev) {
            for (std::uint32_t bi = 0; bi < buf.BlockCount(); ++bi) {
                const vk::DescriptorBufferInfo bii(
                    buf.GetBlockArray(bi), 0, buf.BlockSize());
                vk::WriteDescriptorSet w{};
                w.dstSet = ds;
                w.dstBinding = binding;
                w.dstArrayElement = bi;
                w.descriptorCount = 1;
                w.descriptorType = desc_type;
                w.pBufferInfo = &bii;
                dev.updateDescriptorSets(w, nullptr);
            }
        }
    } // anonymous namespace

void SceneRenderer::PrepareCompute(vk::CommandBuffer /*cmd*/,
                                    VulkanEngine::ComponentRegistry& /*reg*/,
                                    const glm::mat4& /*vm*/, const glm::mat4& /*pm*/,
                                    std::uint32_t, std::uint32_t, std::uint32_t fi) {
    const std::uint32_t f = fi % FRAMES_IN_FLIGHT;
    auto& fr = frames_[f];
    const auto& dev = backend_->GetDevice();

    const std::uint32_t total = current_entity_count_;
    if (total == 0) {
        LOGIFACE_LOG(debug, "PrepareCompute: current_entity_count_ is 0, no work to do");
    }

    fr.compact_dynamic.EnsureCapacity(total);
    fr.compact_static.EnsureCapacity(total);
    fr.bounding_spheres.EnsureCapacity(total);
    fr.bounding_obb.EnsureCapacity(total);
    fr.submesh_vertex_data.EnsureCapacity(total);
    fr.submesh_cull.EnsureCapacity(total);

    constexpr vk::DrawIndirectCommand zero_cmd{ 0, 1, 0, 0 };
    fr.draw_count_buffer.Upload(&zero_cmd, sizeof(zero_cmd));

    // Zero intermediate buffer
    {
        auto* p = fr.intermediate_buffer.Map(0, fr.intermediate_buffer.GetSize());
        if (p) {
            std::memset(p, 0, fr.intermediate_buffer.GetSize());
            fr.intermediate_buffer.Unmap();
        }
    }

    // Zero technique_draw_commands buffer (always, used by fallback path)
    {
        auto* p = fr.technique_draw_commands.Map(0, fr.technique_draw_commands.GetSize());
        if (p) {
            std::memset(p, 0, fr.technique_draw_commands.GetSize());
            fr.technique_draw_commands.Unmap();
        }
    }

    // Zero tech counts and offsets buffers (legacy fallback path)
    {
        auto* p = fr.tech_counts_buffer.Map(0, fr.tech_counts_buffer.GetSize());
        if (p) {
            std::memset(p, 0, fr.tech_counts_buffer.GetSize());
            fr.tech_counts_buffer.Unmap();
        }
    }
    {
        auto* p = fr.tech_offsets_buffer.Map(0, fr.tech_offsets_buffer.GetSize());
        if (p) {
            std::memset(p, 0, fr.tech_offsets_buffer.GetSize());
            fr.tech_offsets_buffer.Unmap();
        }
    }

    LOGIFACE_LOG(trace, "PrepareCompute: binding compact_dynamic blocks=" +
                 std::to_string(fr.compact_dynamic.BlockCount()) +
                 " compact_static blocks=" + std::to_string(fr.compact_static.BlockCount()) +
                 " total=" + std::to_string(total));

    WriteBlocks(fr.expand_set.GetHandle(), 0, fr.compact_dynamic,
                vk::DescriptorType::eStorageBuffer, dev);
    WriteBlocks(fr.expand_set.GetHandle(), 1, fr.compact_static,
                vk::DescriptorType::eStorageBuffer, dev);
    WriteBlocks(fr.expand_set.GetHandle(), 2, fr.submesh_vertex_data,
                vk::DescriptorType::eStorageBuffer, dev);
    WriteBlocks(fr.expand_set.GetHandle(), 3, fr.submesh_cull,
                vk::DescriptorType::eStorageBuffer, dev);

    {
        const vk::DescriptorBufferInfo bi(*fr.indirection_buffer.GetBuffer(), 0, vk::WholeSize);
        vk::WriteDescriptorSet w{};
        w.dstSet = fr.expand_set.GetHandle();
        w.dstBinding = 4;
        w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer;
        w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }
    {
        const vk::DescriptorBufferInfo bi(*fr.draw_count_buffer.GetBuffer(), 0, sizeof(std::uint32_t));
        vk::WriteDescriptorSet w{};
        w.dstSet = fr.expand_set.GetHandle();
        w.dstBinding = 5;
        w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer;
        w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }

    WriteBlocks(fr.submesh_vertex_set.GetHandle(), 0, fr.submesh_vertex_data,
                vk::DescriptorType::eStorageBuffer, dev);
    WriteBlocks(fr.occlusion_set.GetHandle(), 0, fr.submesh_vertex_data,
                vk::DescriptorType::eStorageBuffer, dev);
    WriteBlocks(fr.occlusion_set.GetHandle(), 1, fr.submesh_cull,
                vk::DescriptorType::eStorageBuffer, dev);
    WriteBlocks(fr.occlusion_set.GetHandle(), 2, fr.bounding_spheres,
                vk::DescriptorType::eStorageBuffer, dev);
    WriteBlocks(fr.occlusion_set.GetHandle(), 4, fr.bounding_obb,
                vk::DescriptorType::eStorageBuffer, dev);
    {
        const vk::DescriptorImageInfo hiz_info(
            **hiz_sampler_, *fr.hiz_full_view,
            vk::ImageLayout::eShaderReadOnlyOptimal);
        vk::WriteDescriptorSet w{};
        w.dstSet = fr.occlusion_set.GetHandle();
        w.dstBinding = 3;
        w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eCombinedImageSampler;
        w.pImageInfo = &hiz_info;
        dev.updateDescriptorSets(w, nullptr);
    }
}

void SceneRenderer::DepthPrepass(vk::CommandBuffer cmd, std::uint32_t w, std::uint32_t h, std::uint32_t fi) {
    auto& fr = frames_[fi % FRAMES_IN_FLIGHT];
    if (!depth_pipeline_) {
        LOGIFACE_LOG(warn, "DepthPrepass: depth_pipeline_ is null, skipping");
        return;
    }
    if (!current_entity_count_) {
        LOGIFACE_LOG(debug, "DepthPrepass: current_entity_count_ is 0, skipping");
        return;
    }
    LOGIFACE_LOG(trace, "DepthPrepass: submesh_count=" + std::to_string(current_entity_count_) +
                 " (" + std::to_string(w) + "x" + std::to_string(h) + ")");
    cmd.setViewport(0, vk::Viewport(0, static_cast<float>(h), static_cast<float>(w),
                                     -static_cast<float>(h), 0, 1));
    cmd.setScissor(0, vk::Rect2D({0, 0}, {w, h}));
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *depth_pipeline_);
    const std::array<vk::DescriptorSet, 4> ds{
        empty_sets_[fi % FRAMES_IN_FLIGHT].GetHandle(),
        fr.submesh_vertex_set.GetHandle(),
        static_cast<vk::DescriptorSet>(*fr.bindless_vertex_set),
        *fr.depth_indirection_set
    };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *depth_pipeline_layout_,
                             0, ds, {});
    cmd.drawIndirect(*fr.draw_count_buffer.GetBuffer(), 0, 1,
                     sizeof(vk::DrawIndirectCommand));
}

void SceneRenderer::Render(vk::CommandBuffer cmd,
                            VulkanEngine::ComponentRegistry&,
                            VulkanEngine::TechniqueManager::TechniqueManager& tm,
                            VulkanEngine::BindlessManager::BindlessManager& bm,
                            const glm::mat4&, const glm::mat4&,
                            std::uint32_t w, std::uint32_t h, std::uint32_t fi) {
    if (!w || !h) return;
    if (!current_entity_count_) {
        LOGIFACE_LOG(debug, "Render: current_entity_count_ is 0, skipping");
        return;
    }
    auto& fr = frames_[fi % FRAMES_IN_FLIGHT];
    const auto& dev = backend_->GetDevice();

    // Rebind indirection set to compacted buffer for main pass
    {
        const vk::DescriptorBufferInfo bi(
            *fr.compacted_indirection_buffer.GetBuffer(), 0, vk::WholeSize);
        vk::WriteDescriptorSet w{};
        w.dstSet = *fr.indirection_raw_set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer;
        w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }

    cmd.setViewport(0, vk::Viewport(0, static_cast<float>(h), static_cast<float>(w),
                                     -static_cast<float>(h), 0, 1));
    cmd.setScissor(0, vk::Rect2D({0, 0}, {w, h}));

    // Engine descriptor set handles (used by both legacy and BaseTechnique paths)
    const vk::DescriptorSet engine_set0 = bm.GetDescriptorSet();
    const vk::DescriptorSet engine_set1 = fr.submesh_vertex_set.GetHandle();
    const vk::DescriptorSet engine_set2 = static_cast<vk::DescriptorSet>(*fr.bindless_vertex_set);
    const vk::DescriptorSet engine_set3 = *fr.indirection_raw_set;

    LOGIFACE_LOG(trace, "RenderMain: submesh_count=" + std::to_string(current_entity_count_) +
                 " techniques=" + std::to_string(tm.GetTechniqueCount()));

    for (uint16_t t = 0; t < tm.GetTechniqueCount(); ++t) {
        // Try BaseTechnique first (new path)
        auto* tech = tm.GetTechnique(t);
        if (tech) {
            auto pipeline = tech->GetPipeline();
            auto layout = tech->GetPipelineLayout();
            if (!pipeline || !layout) continue;

            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

            // Build descriptor set array for this technique
            // Engine sets 0-3 at slots 0-3, then custom sets
            std::array<vk::DescriptorSet, 16> ds{};
            std::uint32_t slot = 0;
            ds[slot++] = engine_set0;  // set 0: bindless textures
            ds[slot++] = engine_set1;  // set 1: submesh vertex data
            ds[slot++] = engine_set2;  // set 2: raw vertex buffers
            ds[slot++] = engine_set3;  // set 3: indirection

            // Custom sets (BlockArrays + Shared buffers)
            // Note: GetDescriptorSet() for BlockArrays is not yet implemented;
            // this is a placeholder for the full dynamic binding.
            // For now, custom block arrays are not bound here.

            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout,
                                    0, {ds.data(), slot}, {});

            const vk::DeviceSize draw_cmd_offset =
                static_cast<vk::DeviceSize>(t) * sizeof(vk::DrawIndirectCommand);
            cmd.drawIndirect(*fr.technique_draw_commands.GetBuffer(),
                              draw_cmd_offset, 1, sizeof(vk::DrawIndirectCommand));
            continue;
        }

        // Legacy path (GraphicsPipeline)
        auto* pm = tm.GetGraphicsPipeline(t);
        if (!pm) continue;
        auto* pl = pm->GetPipeline();
        auto* layout = pm->GetPipelineLayout();
        if (!pl || !layout) continue;

        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pl);

        const std::array<vk::DescriptorSet, 4> ds{
            engine_set0, engine_set1, engine_set2, engine_set3
        };
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 0, ds, {});

        const vk::DeviceSize draw_cmd_offset =
            static_cast<vk::DeviceSize>(t) * sizeof(vk::DrawIndirectCommand);
        cmd.drawIndirect(*fr.technique_draw_commands.GetBuffer(),
                          draw_cmd_offset, 1, sizeof(vk::DrawIndirectCommand));
        LOGIFACE_LOG(trace, "  technique[" + std::to_string(t) + "] drawIndirect offset=" +
                     std::to_string(draw_cmd_offset));
    }
}

void SceneRenderer::DispatchExpand(vk::CommandBuffer cmd, std::uint32_t cnt,
                                    const glm::mat4& vp, std::uint32_t fi) {
    auto& fr = frames_[fi % FRAMES_IN_FLIGHT];
    if (!cnt) {
        LOGIFACE_LOG(debug, "DispatchExpand: cnt is 0, skipping");
        return;
    }
    LOGIFACE_LOG(trace, "DispatchExpand: submeshes=" + std::to_string(cnt));
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *expand_pipeline_);
    const std::array<vk::DescriptorSet, 2> ds{
        fr.expand_set.GetHandle(),
        static_cast<vk::DescriptorSet>(*fr.bindless_index_set)
    };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *expand_pipeline_layout_,
                             0, ds, {});
    ExpandPC pc{ vp, cnt, 0, 0 };
    cmd.pushConstants(*expand_pipeline_layout_, vk::ShaderStageFlagBits::eCompute,
                       0, sizeof(ExpandPC), &pc);
    cmd.dispatch((cnt + 63) / 64, 1, 1);
}

void SceneRenderer::DispatchHiZGen(vk::CommandBuffer cmd, std::uint32_t w, std::uint32_t h,
                                    std::uint32_t fi, std::uint32_t) {
    auto& fr = frames_[fi % FRAMES_IN_FLIGHT];
    if (!current_entity_count_) {
        LOGIFACE_LOG(debug, "DispatchHiZGen: current_entity_count_ is 0, skipping");
        return;
    }
    if (!fr.hiz_set.IsValid()) {
        LOGIFACE_LOG(debug, "DispatchHiZGen: hiz_set invalid, skipping");
        return;
    }
    if (w != depth_width_ || h != depth_height_) {
        LOGIFACE_LOG(debug, "DispatchHiZGen: dimensions changed (" + std::to_string(w) + "x" +
                     std::to_string(h) + " != " + std::to_string(depth_width_) + "x" +
                     std::to_string(depth_height_) + "), skipping");
        return;
    }
    LOGIFACE_LOG(trace, "DispatchHiZGen: " + std::to_string(w) + "x" + std::to_string(h) +
                 " mips=" + std::to_string(hiz_mip_count_));
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *hiz_pipeline_);
    const std::array<vk::DescriptorSet, 1> ds{ fr.hiz_set.GetHandle() };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *hiz_pipeline_layout_,
                             0, ds, {});
    for (std::uint32_t bl = 0; bl < hiz_mip_count_ - 1; bl += HIZ_BATCH) {
        const std::uint32_t bc = std::min(HIZ_BATCH, hiz_mip_count_ - bl);
        const std::uint32_t sw = w >> bl;
        const std::uint32_t sh = h >> bl;
        HiZPC pc{ bl, sw, sh, bc };
        cmd.pushConstants(*hiz_pipeline_layout_, vk::ShaderStageFlagBits::eCompute,
                           0, sizeof(HiZPC), &pc);
        cmd.dispatch((sw + 15) / 16, (sh + 15) / 16, 1);
        vk::MemoryBarrier hiz_mb{};
        hiz_mb.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        hiz_mb.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                            vk::PipelineStageFlagBits::eComputeShader,
                            {}, hiz_mb, {}, {});
    }
}

void SceneRenderer::DispatchOcclusion(vk::CommandBuffer cmd, std::uint32_t fi) {
    auto& fr = frames_[fi % FRAMES_IN_FLIGHT];
    if (!current_entity_count_) {
        LOGIFACE_LOG(debug, "DispatchOcclusion: current_entity_count_ is 0, skipping");
        return;
    }
    if (!fr.occlusion_set.IsValid()) {
        LOGIFACE_LOG(debug, "DispatchOcclusion: occlusion_set invalid, skipping");
        return;
    }
    LOGIFACE_LOG(trace, "DispatchOcclusion: submeshes=" + std::to_string(current_entity_count_));

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *occlusion_pipeline_);
    const std::array<vk::DescriptorSet, 1> ds{ fr.occlusion_set.GetHandle() };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *occlusion_pipeline_layout_,
                             0, ds, {});
    const std::uint32_t hiz_w = (depth_width_ + 1) / 2;
    const std::uint32_t hiz_h = (depth_height_ + 1) / 2;
    OccPC pc{ current_entity_count_, 1, hiz_w, hiz_h };
    cmd.pushConstants(*occlusion_pipeline_layout_, vk::ShaderStageFlagBits::eCompute,
                       0, sizeof(OccPC), &pc);
    cmd.dispatch((current_entity_count_ + 63) / 64, 1, 1);
}

void SceneRenderer::DispatchCollect(vk::CommandBuffer cmd, std::uint32_t fi) {
    auto& fr = frames_[fi % FRAMES_IN_FLIGHT];
    if (!current_entity_count_) {
        LOGIFACE_LOG(debug, "DispatchCollect: current_entity_count_ is 0, skipping");
        return;
    }
    const auto& dev = backend_->GetDevice();
    LOGIFACE_LOG(trace, "DispatchCollect: submeshes=" + std::to_string(current_entity_count_) +
                 " techniques=" + std::to_string(MAX_TECHNIQUES));

    // Write set 6 bindings (shared between count + compact)
    WriteBlocks(fr.collect_set.GetHandle(), 0, fr.submesh_cull,
                vk::DescriptorType::eStorageBuffer, dev);
    {
        const vk::DescriptorBufferInfo bi(
            *fr.indirection_buffer.GetBuffer(), 0, vk::WholeSize);
        vk::WriteDescriptorSet w{};
        w.dstSet = fr.collect_set.GetHandle();
        w.dstBinding = 1;
        w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer;
        w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }
    {
        const vk::DescriptorBufferInfo bi(
            *fr.compacted_indirection_buffer.GetBuffer(), 0, vk::WholeSize);
        vk::WriteDescriptorSet w{};
        w.dstSet = fr.collect_set.GetHandle();
        w.dstBinding = 2;
        w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer;
        w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }
    {
        const vk::DescriptorBufferInfo bi(
            *fr.intermediate_buffer.GetBuffer(), 0, fr.intermediate_buffer.GetSize());
        vk::WriteDescriptorSet w{};
        w.dstSet = fr.collect_set.GetHandle();
        w.dstBinding = 3;
        w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer;
        w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }

    // Write set 7 bindings (write shader - intermediate buffer at binding 0)
    {
        const vk::DescriptorBufferInfo bi(
            *fr.intermediate_buffer.GetBuffer(), 0, fr.intermediate_buffer.GetSize());
        vk::WriteDescriptorSet w{};
        w.dstSet = fr.collect_write_set.GetHandle();
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer;
        w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }

    if (dgc_available_) {
        {
            const vk::DescriptorBufferInfo seq_bi(
                *fr.dgc_sequence_buffer.GetBuffer(), 0, fr.dgc_sequence_buffer.GetSize());
            vk::WriteDescriptorSet w2{};
            w2.dstSet = fr.collect_write_set.GetHandle();
            w2.dstBinding = 1;
            w2.descriptorCount = 1;
            w2.descriptorType = vk::DescriptorType::eStorageBuffer;
            w2.pBufferInfo = &seq_bi;
            dev.updateDescriptorSets(w2, nullptr);
        }
        {
            const vk::DescriptorBufferInfo cnt_bi(
                *fr.dgc_count_buffer.GetBuffer(), 0, 4);
            vk::WriteDescriptorSet w2{};
            w2.dstSet = fr.collect_write_set.GetHandle();
            w2.dstBinding = 2;
            w2.descriptorCount = 1;
            w2.descriptorType = vk::DescriptorType::eStorageBuffer;
            w2.pBufferInfo = &cnt_bi;
            dev.updateDescriptorSets(w2, nullptr);
        }
    } else {
        {
            const vk::DescriptorBufferInfo cnt_bi(
                *fr.tech_counts_buffer.GetBuffer(), 0, fr.tech_counts_buffer.GetSize());
            vk::WriteDescriptorSet w{};
            w.dstSet = fr.collect_write_set.GetHandle();
            w.dstBinding = 1;
            w.descriptorCount = 1;
            w.descriptorType = vk::DescriptorType::eStorageBuffer;
            w.pBufferInfo = &cnt_bi;
            dev.updateDescriptorSets(w, nullptr);
        }
        {
            const vk::DescriptorBufferInfo off_bi(
                *fr.tech_offsets_buffer.GetBuffer(), 0, fr.tech_offsets_buffer.GetSize());
            vk::WriteDescriptorSet w{};
            w.dstSet = fr.collect_write_set.GetHandle();
            w.dstBinding = 2;
            w.descriptorCount = 1;
            w.descriptorType = vk::DescriptorType::eStorageBuffer;
            w.pBufferInfo = &off_bi;
            dev.updateDescriptorSets(w, nullptr);
        }
        {
            const vk::DescriptorBufferInfo cmd_bi(
                *fr.technique_draw_commands.GetBuffer(), 0, fr.technique_draw_commands.GetSize());
            vk::WriteDescriptorSet w{};
            w.dstSet = fr.collect_write_set.GetHandle();
            w.dstBinding = 3;
            w.descriptorCount = 1;
            w.descriptorType = vk::DescriptorType::eStorageBuffer;
            w.pBufferInfo = &cmd_bi;
            dev.updateDescriptorSets(w, nullptr);
        }
    }

    // Pass 0: count visible indices per technique
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *collect_pipeline_);
    {
        const std::array<vk::DescriptorSet, 1> ds1{ fr.collect_set.GetHandle() };
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *collect_pipeline_layout_,
                                 0, ds1, {});
    }

    CollectPC pc0{ current_entity_count_, 0, MAX_TECHNIQUES, 0 };
    cmd.pushConstants(*collect_pipeline_layout_, vk::ShaderStageFlagBits::eCompute,
                       0, sizeof(CollectPC), &pc0);
    cmd.dispatch((current_entity_count_ + 255) / 256, 1, 1);

    // Barrier between count and compact
    {
        vk::MemoryBarrier mb{};
        mb.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        mb.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        cmd.pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eComputeShader,
            {}, mb, {}, {});
    }

    // Pass 1: compact visible entries + build intermediate buffer
    CollectPC pc1{ current_entity_count_, 0, MAX_TECHNIQUES, 1 };
    cmd.pushConstants(*collect_pipeline_layout_, vk::ShaderStageFlagBits::eCompute,
                       0, sizeof(CollectPC), &pc1);
    cmd.dispatch((current_entity_count_ + 255) / 256, 1, 1);

    // Barrier between compact and write
    {
        vk::MemoryBarrier mb{};
        mb.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        mb.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        cmd.pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eComputeShader,
            {}, mb, {}, {});
    }

    // Pass 2: write final draw data (DGC sequences or legacy draw commands)
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *collect_write_pipeline_);
    {
        const std::array<vk::DescriptorSet, 1> ds2{ fr.collect_write_set.GetHandle() };
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *collect_write_pipeline_layout_,
                                 0, ds2, {});
    }
    WritePC pc2{ current_entity_count_, 0, MAX_TECHNIQUES, 0 };
    cmd.pushConstants(*collect_write_pipeline_layout_, vk::ShaderStageFlagBits::eCompute,
                       0, sizeof(WritePC), &pc2);
    cmd.dispatch(1, 1, 1);
}

void SceneRenderer::InitializeHizFirstFrame(vk::CommandBuffer cmd) {
    if (hiz_initialized_) return;
    for (std::uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i) {
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
