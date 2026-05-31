module;

#include <algorithm>
#include <cstdint>
#include <vector>
#include <array>
#include <cstring>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/matrix_transform.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/quaternion.hpp> //NOLINT(misc-include-cleaner)
#include <vulkan/vulkan_raii.hpp>

module VulkanEngine.SceneRenderer;

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
        struct DrawEntity {
            const VulkanEngine::Components::Transform* t = nullptr;
            const VulkanEngine::Components::MeshReference* m = nullptr;
        };
        struct ExpandPC { glm::mat4 vp; uint32_t cnt; uint32_t p0; uint32_t p1; };
        struct HiZPC { uint32_t bl; uint32_t sw; uint32_t sh; uint32_t tc; };
        struct OccPC { uint32_t cnt; uint32_t refineLevel; uint32_t hizWidth; uint32_t hizHeight; };
        struct CollectPC { uint32_t cnt; uint32_t p0; uint32_t mt; uint32_t pass; };
        struct WritePC { uint32_t cnt; uint32_t p0; uint32_t techniqueCount; uint32_t p1; };
        static constexpr uint32_t HIZ_BATCH = 2;
        static constexpr uint32_t MAX_GATHER = 65536;

        static void WriteBlocks(vk::DescriptorSet ds, uint32_t binding,
                                GpuResources::BlockArray& buf,
                                vk::DescriptorType desc_type,
                                const vk::raii::Device& dev) {
            for (uint32_t bi = 0; bi < buf.BlockCount(); ++bi) {
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
                                    VulkanEngine::ComponentRegistry& reg,
                                    const glm::mat4& /*vm*/, const glm::mat4& /*pm*/,
                                    uint32_t, uint32_t, uint32_t fi) {
    const uint32_t f = fi % FRAMES_IN_FLIGHT;
    auto& fr = frames_[f];
    const auto& dev = backend_->GetDevice();

    std::vector<DrawEntity> ents;
    ents.reserve(MAX_GATHER);
    reg.ForEach<VulkanEngine::Components::MeshReference>(
        [&](VulkanEngine::Components::MeshReference& mr) {
            if (ents.size() >= MAX_GATHER) return;
            auto* owner = mr.GetOwner();
            if (!owner) return;
            auto* transform = owner->GetComponent<VulkanEngine::Components::Transform>();
            if (!transform) return;
            ents.push_back({ transform, &mr });
        });

    uint32_t total = 0;
    for (auto& e : ents) {
        total += (e.m ? e.m->submesh_count : 0);
    }
    if (total == 0) {
        total = static_cast<uint32_t>(ents.size());
    }
    current_entity_count_ = total;
    if (!total) {
        LOGIFACE_LOG(warn, "PrepareCompute: no entities found with MeshReference component");
    } else {
        LOGIFACE_LOG(trace, "PrepareCompute: entities=" + std::to_string(ents.size()) +
                     " total_submeshes=" + std::to_string(total) + " frame=" + std::to_string(fi));
    }

    fr.compact_dynamic.EnsureCapacity(total);
    fr.compact_static.EnsureCapacity(total);
    fr.bounding_spheres.EnsureCapacity(total);
    fr.bounding_obb.EnsureCapacity(total);
    fr.submesh_vertex_data.EnsureCapacity(total);
    fr.submesh_cull.EnsureCapacity(total);

    uint32_t ci = 0;
    for (auto& e : ents) {
        const auto pos = e.t ? e.t->position : glm::vec3(0);
        const auto scale = e.t ? e.t->scale : glm::vec3(1);
        const glm::quat rot = e.t ? e.t->rotation : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
        if (e.m && e.m->submesh_count) {
            for (uint32_t s = 0; s < e.m->submesh_count; ++s) {
                const uint32_t si = e.m->first_submesh + s;
                const auto sm = (si < scene_submeshes_.size())
                    ? scene_submeshes_[si]
                    : VulkanEngine::SubMesh{};

                struct DynamicEntry {
                    float px, py, pz, pad0;
                    float sx, sy, sz, pad1;
                    float rx, ry, rz, rw;
                };
                if (auto* d = static_cast<DynamicEntry*>(fr.compact_dynamic.Get(ci))) {
                    d->px = pos.x; d->py = pos.y; d->pz = pos.z; d->pad0 = 0;
                    d->sx = scale.x; d->sy = scale.y; d->sz = scale.z; d->pad1 = 0;
                    d->rx = rot.x; d->ry = rot.y; d->rz = rot.z; d->rw = rot.w;
                }

                struct StaticEntry {
                    uint32_t index_start_packed;
                    uint32_t index_range;
                    uint32_t technique_texture;
                    uint32_t pad;
                };
                if (auto* s = static_cast<StaticEntry*>(fr.compact_static.Get(ci))) {
                    s->index_start_packed =
                        (static_cast<uint32_t>(e.m->index_buffer_index) << 24) |
                        sm.index_start;
                    s->index_range = sm.index_count;
                    {
                        const auto& mat_def = MaterialManager::MaterialManager::Get().GetMaterial(sm.material_id);
                        s->technique_texture =
                            (static_cast<uint32_t>(mat_def.texture_slot.value) << 16) |
                            mat_def.technique_id.value;
                    }
                    s->pad = 0;
                }
                LOGIFACE_LOG(trace, "  submesh[ci=" + std::to_string(ci) +
                             "]: idx_buf=" + std::to_string(e.m->index_buffer_index) +
                             " idx_start=" + std::to_string(sm.index_start) +
                             " idx_count=" + std::to_string(sm.index_count) +
                             " mat_id=" + std::to_string(sm.material_id.value) +
                             " mat_def.texture_slot.value=" + std::to_string(MaterialManager::MaterialManager::Get().GetMaterial(sm.material_id).texture_slot.value) +
                             " mat_def.technique_id.value=" + std::to_string(MaterialManager::MaterialManager::Get().GetMaterial(sm.material_id).technique_id.value) +
                             " technique_texture=" + std::to_string(
                                 static_cast<const StaticEntry*>(fr.compact_static.Get(ci))->technique_texture));

                {
                    auto* sp = static_cast<glm::vec4*>(fr.bounding_spheres.Get(ci));
                    if (sp) {
                        sp->x = sm.sphere.center.x;
                        sp->y = sm.sphere.center.y;
                        sp->z = sm.sphere.center.z;
                        sp->w = sm.sphere.radius;
                    }
                }
                {
                    struct OBBGPUEntry {
                        float cx, cy, cz, pad0;
                        float ux, uy, uz, hu;
                        float vx, vy, vz, hv;
                        float wx, wy, wz, hw;
                    };
                    if (auto* ob = static_cast<OBBGPUEntry*>(fr.bounding_obb.Get(ci))) {
                        ob->cx = sm.obb.center.x; ob->cy = sm.obb.center.y; ob->cz = sm.obb.center.z; ob->pad0 = 0;
                        ob->ux = sm.obb.axis_u.x; ob->uy = sm.obb.axis_u.y; ob->uz = sm.obb.axis_u.z; ob->hu = sm.obb.half_extent_u;
                        ob->vx = sm.obb.axis_v.x; ob->vy = sm.obb.axis_v.y; ob->vz = sm.obb.axis_v.z; ob->hv = sm.obb.half_extent_v;
                        ob->wx = sm.obb.axis_w.x; ob->wy = sm.obb.axis_w.y; ob->wz = sm.obb.axis_w.z; ob->hw = sm.obb.half_extent_w;
                    }
                }
                ++ci;
            }
        } else {
            struct DynamicEntry {
                float px, py, pz, pad0;
                float sx, sy, sz, pad1;
                float rx, ry, rz, rw;
            };
            if (auto* d = static_cast<DynamicEntry*>(fr.compact_dynamic.Get(ci))) {
                d->px = pos.x; d->py = pos.y; d->pz = pos.z; d->pad0 = 0;
                d->sx = scale.x; d->sy = scale.y; d->sz = scale.z; d->pad1 = 0;
                d->rx = rot.x; d->ry = rot.y; d->rz = rot.z; d->rw = rot.w;
            }

            struct StaticEntry {
                uint32_t index_start_packed;
                uint32_t index_range;
                uint32_t technique_texture;
                uint32_t pad;
            };
            if (auto* s = static_cast<StaticEntry*>(fr.compact_static.Get(ci))) {
                s->index_start_packed = 0;
                s->index_range = 0;
                s->technique_texture = 0;
                s->pad = 0;
            }

            {
                auto* sp = static_cast<glm::vec4*>(fr.bounding_spheres.Get(ci));
                if (sp) {
                    *sp = glm::vec4(0, 0, 0, 1.0f);
                }
            }
            {
                struct OBBGPUEntry {
                    float cx, cy, cz, pad0;
                    float ux, uy, uz, hu;
                    float vx, vy, vz, hv;
                    float wx, wy, wz, hw;
                };
                if (auto* ob = static_cast<OBBGPUEntry*>(fr.bounding_obb.Get(ci))) {
                    ob->cx = 0; ob->cy = 0; ob->cz = 0; ob->pad0 = 0;
                    ob->ux = 1; ob->uy = 0; ob->uz = 0; ob->hu = 1;
                    ob->vx = 0; ob->vy = 1; ob->vz = 0; ob->hv = 1;
                    ob->wx = 0; ob->wy = 0; ob->wz = 1; ob->hw = 1;
                }
            }
            ++ci;
        }
    }

    VkDrawIndirectCommand zero_cmd{ 0, 1, 0, 0 };
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

    WriteBlocks(fr.expand_set.GetHandle(), 0, fr.compact_dynamic,
                vk::DescriptorType::eStorageBuffer, dev);
    WriteBlocks(fr.expand_set.GetHandle(), 1, fr.compact_static,
                vk::DescriptorType::eStorageBuffer, dev);
    WriteBlocks(fr.expand_set.GetHandle(), 2, fr.submesh_vertex_data,
                vk::DescriptorType::eStorageBuffer, dev);
    WriteBlocks(fr.expand_set.GetHandle(), 3, fr.submesh_cull,
                vk::DescriptorType::eStorageBuffer, dev);

    {
        const vk::DescriptorBufferInfo bi(*fr.indirection_buffer.GetBuffer(), 0, VK_WHOLE_SIZE);
        vk::WriteDescriptorSet w{};
        w.dstSet = fr.expand_set.GetHandle();
        w.dstBinding = 4;
        w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer;
        w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }
    {
        const vk::DescriptorBufferInfo bi(*fr.draw_count_buffer.GetBuffer(), 0, sizeof(uint32_t));
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

void SceneRenderer::DepthPrepass(vk::CommandBuffer cmd, uint32_t w, uint32_t h, uint32_t fi) {
    auto& fr = frames_[fi % FRAMES_IN_FLIGHT];
    if (!current_entity_count_ || !depth_pipeline_) return;
    LOGIFACE_LOG(trace, "DepthPrepass: submesh_count=" + std::to_string(current_entity_count_) +
                 " (" + std::to_string(w) + "x" + std::to_string(h) + ")");
    cmd.setViewport(0, vk::Viewport(0, static_cast<float>(h), static_cast<float>(w),
                                     -static_cast<float>(h), 0, 1));
    cmd.setScissor(0, vk::Rect2D({0, 0}, {w, h}));
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *depth_pipeline_);
    const std::array<vk::DescriptorSet, 4> ds{
        empty_sets_[fi % FRAMES_IN_FLIGHT].GetHandle(),
        fr.submesh_vertex_set.GetHandle(),
        static_cast<vk::DescriptorSet>(*bindless_vertex_set_),
        *fr.depth_indirection_set
    };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *depth_pipeline_layout_,
                             0, ds, {});
    cmd.drawIndirect(*fr.draw_count_buffer.GetBuffer(), 0, 1,
                     sizeof(VkDrawIndirectCommand));
}

void SceneRenderer::Render(vk::CommandBuffer cmd,
                            VulkanEngine::ComponentRegistry&,
                            VulkanEngine::TechniqueManager::TechniqueManager& tm,
                            VulkanEngine::BindlessManager::BindlessManager& bm,
                            const glm::mat4&, const glm::mat4&,
                            uint32_t w, uint32_t h, uint32_t fi) {
    if (!w || !h || !current_entity_count_) return;
    auto& fr = frames_[fi % FRAMES_IN_FLIGHT];
    const auto& dev = backend_->GetDevice();

    // Rebind indirection set to compacted buffer for main pass
    {
        const vk::DescriptorBufferInfo bi(
            *fr.compacted_indirection_buffer.GetBuffer(), 0, VK_WHOLE_SIZE);
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

    // Common descriptor sets for all techniques
    const std::array<vk::DescriptorSet, 4> ds{
        bm.GetDescriptorSet(),
        fr.submesh_vertex_set.GetHandle(),
        static_cast<vk::DescriptorSet>(*bindless_vertex_set_),
        *fr.indirection_raw_set
    };

    LOGIFACE_LOG(trace, "RenderMain: submesh_count=" + std::to_string(current_entity_count_) +
                 " techniques=" + std::to_string(tm.GetTechniqueCount()) +
                 " dgc=" + std::to_string(dgc_available_));
    if (dgc_available_) {
        VkPipelineLayout shared_layout = VK_NULL_HANDLE;
        for (uint16_t t = 0; t < tm.GetTechniqueCount(); ++t) {
            auto* pm = tm.GetGraphicsPipeline(t);
            if (pm && pm->GetPipelineLayout()) {
                shared_layout = *pm->GetPipelineLayout();
                break;
            }
        }
        if (!shared_layout) return;

        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, shared_layout,
                                 0, ds, {});

        VkGeneratedCommandsInfoEXT gen_info{};
        gen_info.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT;
        gen_info.shaderStages = VK_SHADER_STAGE_VERTEX_BIT;
        gen_info.indirectExecutionSet = **dgc_execution_set_;
        gen_info.indirectCommandsLayout = **dgc_commands_layout_;
        gen_info.indirectAddress = fr.dgc_sequence_buffer.GetDeviceAddress(dev);
        gen_info.indirectAddressSize = static_cast<uint64_t>(dgc_max_sequence_count_) * 20u;
        gen_info.preprocessAddress = fr.dgc_preprocess_buffer.GetDeviceAddress(dev);
        gen_info.preprocessSize = fr.dgc_preprocess_size;
        gen_info.maxSequenceCount = dgc_max_sequence_count_;
        gen_info.sequenceCountAddress = fr.dgc_count_buffer.GetDeviceAddress(dev);
        gen_info.maxDrawCount = dgc_max_sequence_count_;

        auto* dispatcher = dev.getDispatcher();
        dispatcher->vkCmdPreprocessGeneratedCommandsEXT(
            static_cast<VkCommandBuffer>(cmd),
            &gen_info,
            VK_NULL_HANDLE);
        dispatcher->vkCmdExecuteGeneratedCommandsEXT(
            static_cast<VkCommandBuffer>(cmd),
            VK_FALSE,
            &gen_info);
    } else {
        for (uint16_t t = 0; t < tm.GetTechniqueCount(); ++t) {
            auto* pm = tm.GetGraphicsPipeline(t);
            if (!pm) continue;
            auto* pl = pm->GetPipeline();
            auto* layout = pm->GetPipelineLayout();
            if (!pl || !layout) continue;

            cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pl);
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 0, ds, {});

            const VkDeviceSize draw_cmd_offset =
                static_cast<VkDeviceSize>(t) * sizeof(VkDrawIndirectCommand);
            cmd.drawIndirect(*fr.technique_draw_commands.GetBuffer(),
                              draw_cmd_offset, 1, sizeof(VkDrawIndirectCommand));
            LOGIFACE_LOG(trace, "  technique[" + std::to_string(t) + "] drawIndirect offset=" +
                         std::to_string(draw_cmd_offset));
        }
    }
}

void SceneRenderer::DispatchExpand(vk::CommandBuffer cmd, uint32_t cnt,
                                    const glm::mat4& vp, uint32_t fi) {
    auto& fr = frames_[fi % FRAMES_IN_FLIGHT];
    if (!cnt) return;
    LOGIFACE_LOG(trace, "DispatchExpand: submeshes=" + std::to_string(cnt));
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *expand_pipeline_);
    const std::array<vk::DescriptorSet, 2> ds{
        fr.expand_set.GetHandle(),
        static_cast<vk::DescriptorSet>(*bindless_index_set_)
    };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *expand_pipeline_layout_,
                             0, ds, {});
    ExpandPC pc{ vp, cnt, 0, 0 };
    cmd.pushConstants(*expand_pipeline_layout_, vk::ShaderStageFlagBits::eCompute,
                       0, sizeof(ExpandPC), &pc);
    cmd.dispatch((cnt + 63) / 64, 1, 1);
}

void SceneRenderer::DispatchHiZGen(vk::CommandBuffer cmd, uint32_t w, uint32_t h,
                                    uint32_t fi, uint32_t) {
    auto& fr = frames_[fi % FRAMES_IN_FLIGHT];
    if (!current_entity_count_ || !fr.hiz_set.IsValid()) return;
    if (w != depth_width_ || h != depth_height_) return;
    LOGIFACE_LOG(trace, "DispatchHiZGen: " + std::to_string(w) + "x" + std::to_string(h) +
                 " mips=" + std::to_string(hiz_mip_count_));
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *hiz_pipeline_);
    const std::array<vk::DescriptorSet, 1> ds{ fr.hiz_set.GetHandle() };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *hiz_pipeline_layout_,
                             0, ds, {});
    for (uint32_t bl = 0; bl < hiz_mip_count_ - 1; bl += HIZ_BATCH) {
        const uint32_t bc = std::min(HIZ_BATCH, hiz_mip_count_ - bl);
        const uint32_t sw = w >> bl;
        const uint32_t sh = h >> bl;
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

void SceneRenderer::DispatchOcclusion(vk::CommandBuffer cmd, uint32_t fi) {
    auto& fr = frames_[fi % FRAMES_IN_FLIGHT];
    if (!current_entity_count_ || !fr.occlusion_set.IsValid()) return;
    LOGIFACE_LOG(trace, "DispatchOcclusion: submeshes=" + std::to_string(current_entity_count_));

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *occlusion_pipeline_);
    const std::array<vk::DescriptorSet, 1> ds{ fr.occlusion_set.GetHandle() };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *occlusion_pipeline_layout_,
                             0, ds, {});
    const uint32_t hiz_w = (depth_width_ + 1) / 2;
    const uint32_t hiz_h = (depth_height_ + 1) / 2;
    OccPC pc{ current_entity_count_, 1, hiz_w, hiz_h };
    cmd.pushConstants(*occlusion_pipeline_layout_, vk::ShaderStageFlagBits::eCompute,
                       0, sizeof(OccPC), &pc);
    cmd.dispatch((current_entity_count_ + 63) / 64, 1, 1);
}

void SceneRenderer::DispatchCollect(vk::CommandBuffer cmd, uint32_t fi) {
    auto& fr = frames_[fi % FRAMES_IN_FLIGHT];
    if (!current_entity_count_) return;
    const auto& dev = backend_->GetDevice();
    LOGIFACE_LOG(trace, "DispatchCollect: submeshes=" + std::to_string(current_entity_count_) +
                 " techniques=" + std::to_string(MAX_TECHNIQUES));

    // Write set 6 bindings (shared between count + compact)
    WriteBlocks(fr.collect_set.GetHandle(), 0, fr.submesh_cull,
                vk::DescriptorType::eStorageBuffer, dev);
    {
        const vk::DescriptorBufferInfo bi(
            *fr.indirection_buffer.GetBuffer(), 0, VK_WHOLE_SIZE);
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
            *fr.compacted_indirection_buffer.GetBuffer(), 0, VK_WHOLE_SIZE);
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
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        auto& f = frames_[i];
        const vk::ImageSubresourceRange hiz_range(
            vk::ImageAspectFlagBits::eColor, 0, hiz_mip_count_, 0, 1);
        const vk::ImageMemoryBarrier imb(
            vk::AccessFlagBits::eNone, vk::AccessFlagBits::eTransferWrite,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
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
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
            *f.hiz_image, hiz_range);
        cmd.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eComputeShader,
            {}, {}, {}, post_clear);
    }
    hiz_initialized_ = true;
}

} // namespace VulkanEngine::SceneRenderer
