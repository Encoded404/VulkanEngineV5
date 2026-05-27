module;

#include <algorithm>
#include <cstdint>
#include <vector>
#include <array>
#include <exception>
#include <string>

#include <vulkan/vulkan_raii.hpp>
#include <logging/logging.hpp>

module VulkanEngine.SceneRenderer;

import VulkanBackend.Component;
import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanBackend.Utils.MemoryUtils;
import VulkanBackend.Utils.VulkanDebugUtils;
import VulkanEngine.GpuResources;
import VulkanEngine.StandardMeshPipeline;

namespace VulkanEngine::SceneRenderer {

SceneRenderer::~SceneRenderer() {
    Shutdown();
}

bool SceneRenderer::Initialize(VulkanEngine::Runtime::IVulkanBootstrap& be,
                                VulkanEngine::GpuResources::DeviceBufferHeap& vh,
                                uint32_t tvc, uint32_t tic) {
    backend_ = &be;
    const auto& dev = be.GetDevice();
    LOGIFACE_LOG(info, "SR init: " + std::to_string(tvc) + "v " + std::to_string(tic) + "i");
    const uint32_t idxc = std::max(tic, 1u);
    total_index_count_ = idxc;

    dgc_available_ = be.IsDgcAvailable();
    if (dgc_available_) {
        dgc_max_sequence_count_ = std::min(be.GetMaxDgcSequenceCount(), DGC_MAX_SEQUENCES);
        LOGIFACE_LOG(info, "DGC available, max sequences: " + std::to_string(dgc_max_sequence_count_));
    } else {
        LOGIFACE_LOG(info, "DGC not available, using fallback path");
    }

    // Set 1: SubmeshVertexData (block array, simple layout)
    {
        std::array<vk::DescriptorSetLayoutBinding, 1> bs{};
        bs[0].binding = 0;
        bs[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        bs[0].descriptorCount = MAX_BLOCKS;
        bs[0].stageFlags = vk::ShaderStageFlagBits::eVertex |
                           vk::ShaderStageFlagBits::eCompute;
        submesh_vertex_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(
            dev, vk::DescriptorSetLayoutCreateInfo{{}, static_cast<uint32_t>(bs.size()), bs.data()});
        VulkanEngine::Utils::SetVulkanObjectName(dev, *submesh_vertex_layout_, "submesh-vertex-layout");
        GpuResources::DescriptorPoolConfig pc{};
        pc.max_sets = FRAMES_IN_FLIGHT;
        pc.max_storage_buffers = FRAMES_IN_FLIGHT * MAX_BLOCKS;
        submesh_vertex_pool_ = GpuResources::DescriptorPool::Create(be, pc);
        submesh_vertex_pool_->SetDebugName(dev, "submesh-vertex-pool");
    }

    // Set 2: Vertex buffers (bindless, update-after-bind)
    {
        std::array<vk::DescriptorSetLayoutBinding, 1> bs{};
        bs[0].binding = 0;
        bs[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        bs[0].descriptorCount = MAX_VERTEX_BUFFERS;
        bs[0].stageFlags = vk::ShaderStageFlagBits::eVertex;
        auto flags = vk::DescriptorBindingFlagBits::ePartiallyBound |
                     vk::DescriptorBindingFlagBits::eUpdateAfterBind |
                     vk::DescriptorBindingFlagBits::eVariableDescriptorCount;
        vk::DescriptorSetLayoutBindingFlagsCreateInfo bind_flags{};
        bind_flags.bindingCount = 1;
        bind_flags.pBindingFlags = &flags;
        vk::DescriptorSetLayoutCreateInfo layout_ci{};
        layout_ci.flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
        layout_ci.pNext = &bind_flags;
        layout_ci.bindingCount = 1;
        layout_ci.pBindings = bs.data();
        raw_vertex_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(dev, layout_ci);
        VulkanEngine::Utils::SetVulkanObjectName(dev, *raw_vertex_layout_, "raw-vertex-layout");

        const vk::DescriptorPoolSize ps{
            vk::DescriptorType::eStorageBuffer, MAX_VERTEX_BUFFERS
        };
        vk::DescriptorPoolCreateInfo pool_ci{};
        pool_ci.flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind |
                        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        pool_ci.maxSets = 1;
        pool_ci.poolSizeCount = 1;
        pool_ci.pPoolSizes = &ps;
        raw_vertex_pool_ = std::make_unique<vk::raii::DescriptorPool>(dev, pool_ci);
        VulkanEngine::Utils::SetVulkanObjectName(dev, *raw_vertex_pool_, "raw-vertex-pool");

        {
            const uint32_t var_desc_count = 1;
            vk::DescriptorSetVariableDescriptorCountAllocateInfo var_desc{};
            var_desc.descriptorSetCount = 1;
            var_desc.pDescriptorCounts = &var_desc_count;
            vk::DescriptorSetAllocateInfo alloc_ci{};
            alloc_ci.pNext = &var_desc;
            alloc_ci.descriptorPool = **raw_vertex_pool_;
            alloc_ci.descriptorSetCount = 1;
            alloc_ci.pSetLayouts = &**raw_vertex_layout_;
            auto sets = dev.allocateDescriptorSets(alloc_ci);
            bindless_vertex_set_ = std::move(sets[0]);
            VulkanEngine::Utils::SetVulkanObjectName(dev, bindless_vertex_set_, "bindless-vertex-set");
        }

        for (uint32_t bi = 0; bi < vh.GetBufferCount(); ++bi) {
            const vk::DescriptorBufferInfo bii(vh.GetBuffer(bi), 0, VK_WHOLE_SIZE);
            vk::WriteDescriptorSet w{};
            w.dstSet = *bindless_vertex_set_;
            w.dstBinding = 0;
            w.dstArrayElement = bi;
            w.descriptorCount = 1;
            w.descriptorType = vk::DescriptorType::eStorageBuffer;
            w.pBufferInfo = &bii;
            dev.updateDescriptorSets(w, nullptr);
        }
    }

    // Set 3: Indirection buffer (single, update-after-bind for per-frame swapping)
    {
        std::array<vk::DescriptorSetLayoutBinding, 1> bs{};
        bs[0].binding = 0;
        bs[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        bs[0].descriptorCount = 1;
        bs[0].stageFlags = vk::ShaderStageFlagBits::eVertex;
        auto flags = vk::DescriptorBindingFlags(vk::DescriptorBindingFlagBits::eUpdateAfterBind);
        vk::DescriptorSetLayoutBindingFlagsCreateInfo bind_flags{};
        bind_flags.bindingCount = 1;
        bind_flags.pBindingFlags = &flags;
        vk::DescriptorSetLayoutCreateInfo layout_ci{};
        layout_ci.pNext = &bind_flags;
        layout_ci.flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
        layout_ci.bindingCount = 1;
        layout_ci.pBindings = bs.data();
        indirection_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(dev, layout_ci);
        VulkanEngine::Utils::SetVulkanObjectName(dev, *indirection_layout_, "indirection-layout");
        const vk::DescriptorPoolSize indir_ps{
            vk::DescriptorType::eStorageBuffer, FRAMES_IN_FLIGHT * 2
        };
        vk::DescriptorPoolCreateInfo indir_pool_ci{};
        indir_pool_ci.flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind |
                              vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        indir_pool_ci.maxSets = FRAMES_IN_FLIGHT * 2;
        indir_pool_ci.poolSizeCount = 1;
        indir_pool_ci.pPoolSizes = &indir_ps;
        indirection_raw_pool_ = std::make_unique<vk::raii::DescriptorPool>(dev, indir_pool_ci);
        VulkanEngine::Utils::SetVulkanObjectName(dev, *indirection_raw_pool_, "indirection-raw-pool");
    }

    // Index buffer array (bindless, update-after-bind) for expand
    {
        std::array<vk::DescriptorSetLayoutBinding, 1> bs{};
        bs[0].binding = 0;
        bs[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        bs[0].descriptorCount = MAX_INDEX_BUFFERS;
        bs[0].stageFlags = vk::ShaderStageFlagBits::eCompute;
        auto flags = vk::DescriptorBindingFlagBits::ePartiallyBound |
                     vk::DescriptorBindingFlagBits::eUpdateAfterBind |
                     vk::DescriptorBindingFlagBits::eVariableDescriptorCount;
        vk::DescriptorSetLayoutBindingFlagsCreateInfo bind_flags{};
        bind_flags.bindingCount = 1;
        bind_flags.pBindingFlags = &flags;
        vk::DescriptorSetLayoutCreateInfo layout_ci{};
        layout_ci.flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
        layout_ci.pNext = &bind_flags;
        layout_ci.bindingCount = 1;
        layout_ci.pBindings = bs.data();
        bindless_index_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(dev, layout_ci);
        VulkanEngine::Utils::SetVulkanObjectName(dev, *bindless_index_layout_, "bindless-index-layout");

        const vk::DescriptorPoolSize ps{
            vk::DescriptorType::eStorageBuffer, MAX_INDEX_BUFFERS
        };
        vk::DescriptorPoolCreateInfo pool_ci{};
        pool_ci.flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind |
                        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        pool_ci.maxSets = 1;
        pool_ci.poolSizeCount = 1;
        pool_ci.pPoolSizes = &ps;
        bindless_index_pool_ = std::make_unique<vk::raii::DescriptorPool>(dev, pool_ci);
        VulkanEngine::Utils::SetVulkanObjectName(dev, *bindless_index_pool_, "bindless-index-pool");

        {
            const uint32_t var_desc_count = 1;
            vk::DescriptorSetVariableDescriptorCountAllocateInfo var_desc{};
            var_desc.descriptorSetCount = 1;
            var_desc.pDescriptorCounts = &var_desc_count;
            vk::DescriptorSetAllocateInfo alloc_ci{};
            alloc_ci.pNext = &var_desc;
            alloc_ci.descriptorPool = **bindless_index_pool_;
            alloc_ci.descriptorSetCount = 1;
            alloc_ci.pSetLayouts = &**bindless_index_layout_;
            auto sets = dev.allocateDescriptorSets(alloc_ci);
            bindless_index_set_ = std::move(sets[0]);
            VulkanEngine::Utils::SetVulkanObjectName(dev, bindless_index_set_, "bindless-index-set");
        }
    }

    // Set 4: Expand layout (6 bindings)
    {
        std::array<vk::DescriptorSetLayoutBinding, 6> bs{};
        for (uint32_t i = 0; i < 4; ++i) {
            bs[i].binding = i;
            bs[i].descriptorType = vk::DescriptorType::eStorageBuffer;
            bs[i].descriptorCount = MAX_BLOCKS;
            bs[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
        }
        for (uint32_t i = 4; i < 6; ++i) {
            bs[i].binding = i;
            bs[i].descriptorType = vk::DescriptorType::eStorageBuffer;
            bs[i].descriptorCount = 1;
            bs[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
        }
        expand_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(
            dev, vk::DescriptorSetLayoutCreateInfo{
                {}, static_cast<uint32_t>(bs.size()), bs.data() });
        VulkanEngine::Utils::SetVulkanObjectName(dev, *expand_layout_, "expand-layout");
        GpuResources::DescriptorPoolConfig pc{};
        pc.max_sets = FRAMES_IN_FLIGHT;
        pc.max_storage_buffers = FRAMES_IN_FLIGHT * (MAX_BLOCKS * 4 + 2);
        expand_pool_ = GpuResources::DescriptorPool::Create(be, pc);
        expand_pool_->SetDebugName(dev, "expand-pool");
    }

    // Set 5: Occlusion layout (5 bindings)
    {
        std::array<vk::DescriptorSetLayoutBinding, 5> bs{};
        for (uint32_t i = 0; i < 3; ++i) {
            bs[i].binding = i;
            bs[i].descriptorType = vk::DescriptorType::eStorageBuffer;
            bs[i].descriptorCount = MAX_BLOCKS;
            bs[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
        }
        bs[3].binding = 3;
        bs[3].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        bs[3].descriptorCount = 1;
        bs[3].stageFlags = vk::ShaderStageFlagBits::eCompute;
        bs[4].binding = 4;
        bs[4].descriptorType = vk::DescriptorType::eStorageBuffer;
        bs[4].descriptorCount = MAX_BLOCKS;
        bs[4].stageFlags = vk::ShaderStageFlagBits::eCompute;
        occlusion_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(
            dev, vk::DescriptorSetLayoutCreateInfo{
                {}, static_cast<uint32_t>(bs.size()), bs.data() });
        VulkanEngine::Utils::SetVulkanObjectName(dev, *occlusion_layout_, "occlusion-layout");
        GpuResources::DescriptorPoolConfig pc{};
        pc.max_sets = FRAMES_IN_FLIGHT;
        pc.max_storage_buffers = FRAMES_IN_FLIGHT * MAX_BLOCKS * 4;
        pc.max_sampled_images = FRAMES_IN_FLIGHT;
        pc.max_combined_image_samplers = FRAMES_IN_FLIGHT;
        occlusion_pool_ = GpuResources::DescriptorPool::Create(be, pc);
        occlusion_pool_->SetDebugName(dev, "occlusion-pool");
    }

    // Set 6: Collect count + compact layout (4 bindings: cull blocks, full indir, compacted indir, intermediate)
    {
        std::array<vk::DescriptorSetLayoutBinding, 4> bs{};
        bs[0].binding = 0;
        bs[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        bs[0].descriptorCount = MAX_BLOCKS;
        bs[0].stageFlags = vk::ShaderStageFlagBits::eCompute;
        for (uint32_t i = 1; i < 4; ++i) {
            bs[i].binding = i;
            bs[i].descriptorType = vk::DescriptorType::eStorageBuffer;
            bs[i].descriptorCount = 1;
            bs[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
        }
        collect_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(
            dev, vk::DescriptorSetLayoutCreateInfo{
                {}, static_cast<uint32_t>(bs.size()), bs.data() });
        VulkanEngine::Utils::SetVulkanObjectName(dev, *collect_layout_, "collect-layout");
        GpuResources::DescriptorPoolConfig pc{};
        pc.max_sets = FRAMES_IN_FLIGHT;
        pc.max_storage_buffers = FRAMES_IN_FLIGHT * (MAX_BLOCKS + 3);
        collect_pool_ = GpuResources::DescriptorPool::Create(be, pc);
        collect_pool_->SetDebugName(dev, "collect-pool");
    }

    // Set 7: Collect write shader layout (varies by DGC availability)
    {
        if (dgc_available_) {
            std::array<vk::DescriptorSetLayoutBinding, 3> bs{};
            for (uint32_t i = 0; i < 3; ++i) {
                bs[i].binding = i;
                bs[i].descriptorType = vk::DescriptorType::eStorageBuffer;
                bs[i].descriptorCount = 1;
                bs[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
            }
            collect_write_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(
                dev, vk::DescriptorSetLayoutCreateInfo{
                    {}, static_cast<uint32_t>(bs.size()), bs.data() });
        } else {
            std::array<vk::DescriptorSetLayoutBinding, 2> bs{};
            for (uint32_t i = 0; i < 2; ++i) {
                bs[i].binding = i;
                bs[i].descriptorType = vk::DescriptorType::eStorageBuffer;
                bs[i].descriptorCount = 1;
                bs[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
            }
            collect_write_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(
                dev, vk::DescriptorSetLayoutCreateInfo{
                    {}, static_cast<uint32_t>(bs.size()), bs.data() });
        }
        VulkanEngine::Utils::SetVulkanObjectName(dev, *collect_write_layout_, "collect-write-layout");
        GpuResources::DescriptorPoolConfig pc{};
        pc.max_sets = FRAMES_IN_FLIGHT + 1;
        pc.max_storage_buffers = FRAMES_IN_FLIGHT * 5;
        collect_write_pool_ = GpuResources::DescriptorPool::Create(be, pc);
        collect_write_pool_->SetDebugName(dev, "collect-write-pool");
    }

    // Empty set (placeholder for set 0 in depth pass)
    {
        constexpr vk::DescriptorSetLayoutCreateInfo empty_ci{};
        empty_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(dev, empty_ci);
        VulkanEngine::Utils::SetVulkanObjectName(dev, *empty_layout_, "empty-layout");
        GpuResources::DescriptorPoolConfig pc{};
        pc.max_sets = FRAMES_IN_FLIGHT;
        empty_pool_ = GpuResources::DescriptorPool::Create(be, pc);
        empty_pool_->SetDebugName(dev, "empty-pool");
        empty_sets_.reserve(FRAMES_IN_FLIGHT);
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; ++i) {
            auto set = empty_pool_->Allocate(*empty_layout_);
            set.SetDebugName(dev, "empty-set-" + std::to_string(i));
            empty_sets_.push_back(std::move(set));
        }
    }

    // Create all compute/graphics pipelines
    if (!CreateExpandPipeline(be)) return false;

    {
        vk::PipelineRasterizationStateCreateInfo rs{};
        rs.polygonMode = vk::PolygonMode::eFill;
        rs.cullMode = vk::CullModeFlagBits::eFront;
        rs.frontFace = vk::FrontFace::eClockwise;
        rs.lineWidth = 1.0f;
        if (!CreateDepthPipeline(be, rs)) return false;
    }

    if (!CreateHiZPipeline(be)) return false;
    if (!CreateOcclusionPipeline(be)) return false;
    if (!CreateCollectPipelines(be)) return false;

    if (dgc_available_) {
        if (!CreateDegeneratePipeline(be)) return false;
    }

    // Per-frame ring resources
    {
        const uint64_t max_indirection_size =
            static_cast<uint64_t>(idxc) * 8u;
        const uint64_t technique_cmd_size =
            static_cast<uint64_t>(MAX_TECHNIQUES) *
            (4 + 4 + sizeof(VkDrawIndirectCommand));
        const uint64_t intermediate_size =
            static_cast<uint64_t>(MAX_TECHNIQUES) * 8u;
        const uint64_t dgc_seq_size =
            static_cast<uint64_t>(dgc_max_sequence_count_) * 20u;

        auto make_block_config = [](uint32_t entry_size, uint32_t entries_per_block,
                                     vk::BufferUsageFlags extra_usage,
                                     vk::MemoryPropertyFlags memory) {
            GpuResources::BlockArray::Config c{};
            c.entry_size = entry_size;
            c.entries_per_block = entries_per_block;
            c.extra_usage = extra_usage;
            c.memory = memory;
            return c;
        };

        auto* dev_dispatcher = dev.getDispatcher();
        const VkDevice vk_dev = static_cast<VkDevice>(*dev);

        if (dgc_available_) {
            VkIndirectCommandsExecutionSetTokenEXT exec_set_token{};
            exec_set_token.type = VK_INDIRECT_EXECUTION_SET_INFO_TYPE_PIPELINES_EXT;
            exec_set_token.shaderStages = VK_SHADER_STAGE_VERTEX_BIT;

            const uint32_t dgc_stride = 20;
            std::array<VkIndirectCommandsLayoutTokenEXT, 2> dgc_tokens{};
            dgc_tokens[0].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
            dgc_tokens[0].type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_EXECUTION_SET_EXT;
            dgc_tokens[0].data.pExecutionSet = &exec_set_token;
            dgc_tokens[0].offset = 0;
            dgc_tokens[1].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
            dgc_tokens[1].type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_EXT;
            dgc_tokens[1].data = {};
            dgc_tokens[1].offset = 4;

            VkIndirectCommandsLayoutCreateInfoEXT cmd_layout_ci{};
            cmd_layout_ci.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT;
            cmd_layout_ci.shaderStages = VK_SHADER_STAGE_VERTEX_BIT;
            cmd_layout_ci.indirectStride = dgc_stride;
            cmd_layout_ci.pipelineLayout = static_cast<VkPipelineLayout>(*dgc_degenerate_layout_);
            cmd_layout_ci.tokenCount = 2;
            cmd_layout_ci.pTokens = dgc_tokens.data();

            VkIndirectCommandsLayoutEXT raw_layout{};
            dev_dispatcher->vkCreateIndirectCommandsLayoutEXT(vk_dev, &cmd_layout_ci, nullptr, &raw_layout);
            dgc_commands_layout_ = std::make_unique<vk::raii::IndirectCommandsLayoutEXT>(dev, raw_layout);
            VulkanEngine::Utils::SetVulkanObjectName(dev, *dgc_commands_layout_, "dgc-commands-layout");

            VkIndirectExecutionSetPipelineInfoEXT pipeline_info{};
            pipeline_info.sType = VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_PIPELINE_INFO_EXT;
            pipeline_info.initialPipeline = static_cast<VkPipeline>(*dgc_degenerate_pipeline_);
            pipeline_info.maxPipelineCount = dgc_max_sequence_count_;

            VkIndirectExecutionSetInfoEXT exec_set_info{};
            exec_set_info.pPipelineInfo = &pipeline_info;

            VkIndirectExecutionSetCreateInfoEXT exec_set_ci{};
            exec_set_ci.sType = VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_CREATE_INFO_EXT;
            exec_set_ci.type = VK_INDIRECT_EXECUTION_SET_INFO_TYPE_PIPELINES_EXT;
            exec_set_ci.info = exec_set_info;

            VkIndirectExecutionSetEXT raw_exec_set{};
            dev_dispatcher->vkCreateIndirectExecutionSetEXT(vk_dev, &exec_set_ci, nullptr, &raw_exec_set);
            dgc_execution_set_ = std::make_unique<vk::raii::IndirectExecutionSetEXT>(dev, raw_exec_set);
            VulkanEngine::Utils::SetVulkanObjectName(dev, *dgc_execution_set_, "dgc-execution-set");
        }

        for (auto& fr : frames_) {
            fr.compact_dynamic.Initialize(be,
                make_block_config(48, BLOCK_ENTRIES, {},
                    vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent));
            fr.compact_static.Initialize(be,
                make_block_config(16, BLOCK_ENTRIES, {},
                    vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent));
            fr.bounding_spheres.Initialize(be,
                make_block_config(16, BLOCK_ENTRIES, {},
                    vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent));
            fr.bounding_obb.Initialize(be,
                make_block_config(64, BLOCK_ENTRIES, {},
                    vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent));
            fr.submesh_vertex_data.Initialize(be,
                make_block_config(80, BLOCK_ENTRIES,
                    vk::BufferUsageFlagBits::eTransferSrc,
                    vk::MemoryPropertyFlagBits::eDeviceLocal));
            fr.submesh_cull.Initialize(be,
                make_block_config(16, BLOCK_ENTRIES,
                    vk::BufferUsageFlagBits::eTransferSrc,
                    vk::MemoryPropertyFlagBits::eDeviceLocal));

            fr.indirection_buffer = GpuResources::GpuBuffer::Create(be,
                max_indirection_size,
                vk::BufferUsageFlagBits::eStorageBuffer |
                    vk::BufferUsageFlagBits::eTransferDst,
                vk::MemoryPropertyFlagBits::eDeviceLocal);

            VulkanEngine::Utils::SetVulkanObjectName(dev, fr.indirection_buffer.GetBuffer(), VK_OBJECT_TYPE_BUFFER, "indirection-buffer");

            fr.compacted_indirection_buffer = GpuResources::GpuBuffer::Create(be,
                max_indirection_size,
                vk::BufferUsageFlagBits::eStorageBuffer |
                    vk::BufferUsageFlagBits::eTransferDst,
                vk::MemoryPropertyFlagBits::eDeviceLocal);

            VulkanEngine::Utils::SetVulkanObjectName(dev, fr.compacted_indirection_buffer.GetBuffer(), VK_OBJECT_TYPE_BUFFER, "compacted-indirection-buffer");

            fr.draw_count_buffer = GpuResources::GpuBuffer::Create(be,
                sizeof(VkDrawIndirectCommand),
                vk::BufferUsageFlagBits::eStorageBuffer |
                    vk::BufferUsageFlagBits::eIndirectBuffer |
                    vk::BufferUsageFlagBits::eTransferDst,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent);

            VulkanEngine::Utils::SetVulkanObjectName(dev, fr.draw_count_buffer.GetBuffer(), VK_OBJECT_TYPE_BUFFER, "draw-count-buffer");

            fr.intermediate_buffer = GpuResources::GpuBuffer::Create(be,
                intermediate_size,
                vk::BufferUsageFlagBits::eStorageBuffer |
                    vk::BufferUsageFlagBits::eTransferDst,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent);
            VulkanEngine::Utils::SetVulkanObjectName(dev, fr.intermediate_buffer.GetBuffer(), VK_OBJECT_TYPE_BUFFER, "intermediate-buffer");

            if (dgc_available_) {
                const vk::BufferUsageFlags dgc_usage =
                    vk::BufferUsageFlagBits::eStorageBuffer |
                    vk::BufferUsageFlagBits::eIndirectBuffer |
                    vk::BufferUsageFlagBits::eShaderDeviceAddress;
                fr.dgc_sequence_buffer = GpuResources::GpuBuffer::Create(be,
                    dgc_seq_size, dgc_usage,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent);
                VulkanEngine::Utils::SetVulkanObjectName(dev, fr.dgc_sequence_buffer.GetBuffer(), VK_OBJECT_TYPE_BUFFER, "dgc-sequence-buffer");
                fr.dgc_count_buffer = GpuResources::GpuBuffer::Create(be,
                    4, dgc_usage,
                    vk::MemoryPropertyFlagBits::eHostVisible |
                        vk::MemoryPropertyFlagBits::eHostCoherent);
                VulkanEngine::Utils::SetVulkanObjectName(dev, fr.dgc_count_buffer.GetBuffer(), VK_OBJECT_TYPE_BUFFER, "dgc-count-buffer");
                VkGeneratedCommandsMemoryRequirementsInfoEXT mem_req_info{};
                mem_req_info.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT;
                mem_req_info.indirectExecutionSet = **dgc_execution_set_;
                mem_req_info.indirectCommandsLayout = **dgc_commands_layout_;
                mem_req_info.maxSequenceCount = dgc_max_sequence_count_;
                mem_req_info.maxDrawCount = dgc_max_sequence_count_;
                VkMemoryRequirements2 mem_req{};
                mem_req.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
                dev_dispatcher->vkGetGeneratedCommandsMemoryRequirementsEXT(vk_dev, &mem_req_info, &mem_req);
                fr.dgc_preprocess_size = mem_req.memoryRequirements.size;
                fr.dgc_preprocess_buffer = GpuResources::GpuBuffer::Create(be,
                    fr.dgc_preprocess_size,
                    vk::BufferUsageFlagBits::eShaderDeviceAddress,
                    vk::MemoryPropertyFlagBits::eDeviceLocal);
                VulkanEngine::Utils::SetVulkanObjectName(dev, fr.dgc_preprocess_buffer.GetBuffer(), VK_OBJECT_TYPE_BUFFER, "dgc-preprocess-buffer");
            }

            fr.technique_draw_commands = GpuResources::GpuBuffer::Create(be,
                technique_cmd_size,
                vk::BufferUsageFlagBits::eStorageBuffer |
                    vk::BufferUsageFlagBits::eIndirectBuffer |
                    vk::BufferUsageFlagBits::eTransferDst,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent);

            VulkanEngine::Utils::SetVulkanObjectName(dev, fr.technique_draw_commands.GetBuffer(), VK_OBJECT_TYPE_BUFFER, "technique-draw-cmds");

            fr.expand_set = expand_pool_->Allocate(*expand_layout_);
            fr.occlusion_set = occlusion_pool_->Allocate(*occlusion_layout_);
            fr.collect_set = collect_pool_->Allocate(*collect_layout_);
            fr.collect_write_set = collect_write_pool_->Allocate(*collect_write_layout_);
            fr.submesh_vertex_set =
                submesh_vertex_pool_->Allocate(*submesh_vertex_layout_);
            {
                vk::DescriptorSetAllocateInfo alloc_ci{};
                alloc_ci.descriptorPool = **indirection_raw_pool_;
                alloc_ci.descriptorSetCount = 1;
                alloc_ci.pSetLayouts = &**indirection_layout_;
                auto sets = dev.allocateDescriptorSets(alloc_ci);
                fr.indirection_raw_set = std::move(sets[0]);
                VulkanEngine::Utils::SetVulkanObjectName(dev, fr.indirection_raw_set, "indirection-raw-set");
            }
            {
                vk::DescriptorSetAllocateInfo alloc_ci{};
                alloc_ci.descriptorPool = **indirection_raw_pool_;
                alloc_ci.descriptorSetCount = 1;
                alloc_ci.pSetLayouts = &**indirection_layout_;
                auto sets = dev.allocateDescriptorSets(alloc_ci);
                fr.depth_indirection_set = std::move(sets[0]);
                VulkanEngine::Utils::SetVulkanObjectName(dev, fr.depth_indirection_set, "depth-indirection-set");
                const vk::DescriptorBufferInfo bi(*fr.indirection_buffer.GetBuffer(), 0, VK_WHOLE_SIZE);
                vk::WriteDescriptorSet w{};
                w.dstSet = *fr.depth_indirection_set;
                w.dstBinding = 0;
                w.descriptorCount = 1;
                w.descriptorType = vk::DescriptorType::eStorageBuffer;
                w.pBufferInfo = &bi;
                dev.updateDescriptorSets(w, nullptr);
            }
            fr.hiz_set = hiz_pool_->Allocate(*hiz_layout_);

            fr.expand_set.SetDebugName(dev, "expand-set");
            fr.occlusion_set.SetDebugName(dev, "occlusion-set");
            fr.collect_set.SetDebugName(dev, "collect-set");
            fr.collect_write_set.SetDebugName(dev, "collect-write-set");
            fr.submesh_vertex_set.SetDebugName(dev, "submesh-vertex-set");
            fr.hiz_set.SetDebugName(dev, "hiz-set");
        }
    }

    (void)be.GetSwapchainExtent(depth_width_, depth_height_);

    // Create Hi-Z image and sampler for each frame
    {
        const uint32_t hiz_w = (depth_width_ + 1) / 2;
        const uint32_t hiz_h = (depth_height_ + 1) / 2;
        uint32_t max_dim = std::max(hiz_w, hiz_h);
        uint32_t mip_levels = 1;
        while (max_dim > 1) { max_dim >>= 1; ++mip_levels; }
        mip_levels = std::min(mip_levels, MAX_HIZ_MIPS);
        const vk::Format hiz_format = vk::Format::eR32Sfloat;

        vk::SamplerCreateInfo sampler_ci{};
        sampler_ci.magFilter = vk::Filter::eNearest;
        sampler_ci.minFilter = vk::Filter::eNearest;
        sampler_ci.mipmapMode = vk::SamplerMipmapMode::eNearest;
        sampler_ci.minLod = 0.0f;
        sampler_ci.maxLod = static_cast<float>(mip_levels);
        hiz_sampler_ = std::make_unique<vk::raii::Sampler>(dev, sampler_ci);
        VulkanEngine::Utils::SetVulkanObjectName(dev, *hiz_sampler_, "hiz-sampler");

        for (auto& fr : frames_) {
            vk::ImageCreateInfo img_ci{};
            img_ci.imageType = vk::ImageType::e2D;
            img_ci.format = hiz_format;
            img_ci.extent = vk::Extent3D(hiz_w, hiz_h, 1);
            img_ci.mipLevels = mip_levels;
            img_ci.arrayLayers = 1;
            img_ci.samples = vk::SampleCountFlagBits::e1;
            img_ci.tiling = vk::ImageTiling::eOptimal;
            img_ci.usage = vk::ImageUsageFlagBits::eStorage |
                           vk::ImageUsageFlagBits::eSampled |
                           vk::ImageUsageFlagBits::eTransferDst;
            img_ci.initialLayout = vk::ImageLayout::eUndefined;
            fr.hiz_image = vk::raii::Image(dev, img_ci);

            VulkanEngine::Utils::SetVulkanObjectName(dev, fr.hiz_image, "hiz-image");

            const vk::MemoryRequirements mem_req = fr.hiz_image.getMemoryRequirements();
            vk::MemoryAllocateInfo alloc_ci{};
            alloc_ci.allocationSize = mem_req.size;
            alloc_ci.memoryTypeIndex = VulkanEngine::Utils::MemoryUtils::FindMemoryType(
                be.GetPhysicalDevice(), mem_req.memoryTypeBits,
                vk::MemoryPropertyFlagBits::eDeviceLocal);
            fr.hiz_memory = vk::raii::DeviceMemory(dev, alloc_ci);
            VulkanEngine::Utils::SetVulkanObjectName(dev, fr.hiz_memory, "hiz-memory");
            fr.hiz_image.bindMemory(*fr.hiz_memory, 0);

            fr.hiz_mip_views.clear();
            fr.hiz_mip_views.reserve(mip_levels);
            for (uint32_t mip = 0; mip < mip_levels; ++mip) {
                vk::ImageViewCreateInfo view_ci{};
                view_ci.image = *fr.hiz_image;
                view_ci.viewType = vk::ImageViewType::e2D;
                view_ci.format = hiz_format;
                view_ci.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
                view_ci.subresourceRange.baseMipLevel = mip;
                view_ci.subresourceRange.levelCount = 1;
                view_ci.subresourceRange.baseArrayLayer = 0;
                view_ci.subresourceRange.layerCount = 1;
                fr.hiz_mip_views.emplace_back(dev, view_ci);
            VulkanEngine::Utils::SetVulkanObjectName(dev, fr.hiz_mip_views.back(), "hiz-mip-view-" + std::to_string(mip));
            }

            vk::ImageViewCreateInfo full_view_ci{};
            full_view_ci.image = *fr.hiz_image;
            full_view_ci.viewType = vk::ImageViewType::e2D;
            full_view_ci.format = hiz_format;
            full_view_ci.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            full_view_ci.subresourceRange.baseMipLevel = 0;
            full_view_ci.subresourceRange.levelCount = mip_levels;
            full_view_ci.subresourceRange.baseArrayLayer = 0;
            full_view_ci.subresourceRange.layerCount = 1;
            fr.hiz_full_view = vk::raii::ImageView(dev, full_view_ci);
            VulkanEngine::Utils::SetVulkanObjectName(dev, fr.hiz_full_view, "hiz-full-view");

            // Bind Hi-Z mip views as storage image array and sampler
            std::vector<vk::DescriptorImageInfo> storage_infos;
            storage_infos.reserve(mip_levels);
            for (uint32_t mip = 0; mip < mip_levels; ++mip) {
                storage_infos.emplace_back(
                    VK_NULL_HANDLE, *fr.hiz_mip_views[mip],
                    vk::ImageLayout::eGeneral);
            }
            {
                vk::WriteDescriptorSet w{};
                w.dstSet = fr.hiz_set.GetHandle();
                w.dstBinding = 2;
                w.dstArrayElement = 0;
                w.descriptorCount = mip_levels;
                w.descriptorType = vk::DescriptorType::eStorageImage;
                w.pImageInfo = storage_infos.data();
                dev.updateDescriptorSets(w, nullptr);
            }
            // Bind hiz mip 0 as placeholder depth input (binding 0). Uses eGeneral
            // layout to match the storage image binding at binding 2 on same subresource.
            {
                const vk::DescriptorImageInfo depth_info(
                    VK_NULL_HANDLE, *fr.hiz_mip_views[0],
                    vk::ImageLayout::eGeneral);
                vk::WriteDescriptorSet w{};
                w.dstSet = fr.hiz_set.GetHandle();
                w.dstBinding = 0;
                w.descriptorCount = 1;
                w.descriptorType = vk::DescriptorType::eSampledImage;
                w.pImageInfo = &depth_info;
                dev.updateDescriptorSets(w, nullptr);
            }
            {
                const vk::DescriptorImageInfo sampler_info(
                    **hiz_sampler_, VK_NULL_HANDLE,
                    vk::ImageLayout::eShaderReadOnlyOptimal);
                vk::WriteDescriptorSet w{};
                w.dstSet = fr.hiz_set.GetHandle();
                w.dstBinding = 1;
                w.descriptorCount = 1;
                w.descriptorType = vk::DescriptorType::eSampler;
                w.pImageInfo = &sampler_info;
                dev.updateDescriptorSets(w, nullptr);
            }
        }
        hiz_mip_count_ = mip_levels;
    }

    LOGIFACE_LOG(info, "SceneRenderer initialized (DGC=" + std::to_string(dgc_available_) + ")");
    return true;
}

void SceneRenderer::Shutdown() {
    if (backend_) {
        try {
            backend_->GetDevice().waitIdle();
        } catch (const std::exception&) {} //NOLINT(bugprone-empty-catch)
    }
    for (auto& fr : frames_) {
        fr.compact_dynamic.Shutdown();
        fr.compact_static.Shutdown();
        fr.bounding_spheres.Shutdown();
        fr.bounding_obb.Shutdown();
        fr.submesh_vertex_data.Shutdown();
        fr.submesh_cull.Shutdown();
    }
    dgc_execution_set_.reset();
    dgc_commands_layout_.reset();
    dgc_degenerate_pipeline_ = nullptr;
    dgc_degenerate_layout_ = nullptr;
    backend_ = nullptr;
}

vk::DescriptorSetLayout* SceneRenderer::GetSubmeshVertexDataLayout() const {
    return submesh_vertex_layout_
        ? const_cast<vk::DescriptorSetLayout*>(&**submesh_vertex_layout_)
        : nullptr;
}

vk::DescriptorSetLayout* SceneRenderer::GetRawVertexLayout() const {
    return raw_vertex_layout_
        ? const_cast<vk::DescriptorSetLayout*>(&**raw_vertex_layout_)
        : nullptr;
}

vk::DescriptorSetLayout* SceneRenderer::GetIndirectionLayout() const {
    return indirection_layout_
        ? const_cast<vk::DescriptorSetLayout*>(&**indirection_layout_)
        : nullptr;
}

void SceneRenderer::UpdateVertexBufferArrayElement(uint32_t buffer_index,
                                                     vk::Buffer buffer,
                                                     uint64_t size) {
    const vk::DescriptorBufferInfo bii(buffer, 0, size);
    vk::WriteDescriptorSet w{};
    w.dstSet = *bindless_vertex_set_;
    w.dstBinding = 0;
    w.dstArrayElement = buffer_index;
    w.descriptorCount = 1;
    w.descriptorType = vk::DescriptorType::eStorageBuffer;
    w.pBufferInfo = &bii;
    backend_->GetDevice().updateDescriptorSets(w, nullptr);
}

void SceneRenderer::UpdateIndexBufferArrayElement(uint32_t buffer_index,
                                                    vk::Buffer buffer,
                                                    uint64_t size) {
    const vk::DescriptorBufferInfo bii(buffer, 0, size);
    vk::WriteDescriptorSet w{};
    w.dstSet = *bindless_index_set_;
    w.dstBinding = 0;
    w.dstArrayElement = buffer_index;
    w.descriptorCount = 1;
    w.descriptorType = vk::DescriptorType::eStorageBuffer;
    w.pBufferInfo = &bii;
    backend_->GetDevice().updateDescriptorSets(w, nullptr);
}

void SceneRenderer::UpdateBlockArrayDescriptor(vk::DescriptorSet desc_set,
                                                 uint32_t binding,
                                                 GpuResources::BlockArray& buf,
                                                 vk::DescriptorType desc_type) {
    if (!backend_) return;
    auto& dev = backend_->GetDevice();
    for (uint32_t bi = 0; bi < buf.BlockCount(); ++bi) {
        const vk::DescriptorBufferInfo bii(buf.GetBlockArray(bi), 0, buf.BlockSize());
        vk::WriteDescriptorSet w{};
        w.dstSet = desc_set;
        w.dstBinding = binding;
        w.dstArrayElement = bi;
        w.descriptorCount = 1;
        w.descriptorType = desc_type;
        w.pBufferInfo = &bii;
        dev.updateDescriptorSets(w, nullptr);
    }
}

void SceneRenderer::UpdateHizDepthBinding(uint32_t frame_index, VkImageView depth_view) {
    auto& fr = frames_[frame_index % FRAMES_IN_FLIGHT];
    if (!backend_) return;
    auto& dev = backend_->GetDevice();
    const vk::DescriptorImageInfo depth_info(
        VK_NULL_HANDLE, depth_view, vk::ImageLayout::eShaderReadOnlyOptimal);
    vk::WriteDescriptorSet w{};
    w.dstSet = fr.hiz_set.GetHandle();
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = vk::DescriptorType::eSampledImage;
    w.pImageInfo = &depth_info;
    dev.updateDescriptorSets(w, nullptr);
}

void SceneRenderer::SetupTechniqueDgcCallback(VulkanEngine::TechniqueManager::TechniqueManager& tm) {
    if (!dgc_available_ || !dgc_execution_set_) return;
    const auto& dev = backend_->GetDevice();
    const VkDevice vk_dev = static_cast<VkDevice>(*dev);
    const VkIndirectExecutionSetEXT exec_set = **dgc_execution_set_;
    auto* dispatcher = dev.getDispatcher();

    tm.SetTechniqueCallback(
        [exec_set, dispatcher, vk_dev](uint16_t id, VkPipeline pipeline, VkPipelineLayout) {
            VkWriteIndirectExecutionSetPipelineEXT write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_INDIRECT_EXECUTION_SET_PIPELINE_EXT;
            write.index = id;
            write.pipeline = pipeline;
            dispatcher->vkUpdateIndirectExecutionSetPipelineEXT(vk_dev, exec_set, 1, &write);
        });

    for (uint16_t t = 0; t < tm.GetTechniqueCount(); ++t) {
        auto* pm = tm.GetGraphicsPipeline(t);
        if (!pm) continue;
        auto* pl = pm->GetPipeline();
        if (!pl) continue;
        VkWriteIndirectExecutionSetPipelineEXT write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_INDIRECT_EXECUTION_SET_PIPELINE_EXT;
        write.index = t;
        write.pipeline = static_cast<VkPipeline>(**pl);
        dispatcher->vkUpdateIndirectExecutionSetPipelineEXT(vk_dev, exec_set, 1, &write);
    }
}

} // namespace VulkanEngine::SceneRenderer
