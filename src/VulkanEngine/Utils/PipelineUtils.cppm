module;

#include <volk.h>
#include <cstdint>
#include <vector>

export module VulkanEngine.Utils.PipelineUtils;

export namespace VulkanEngine::Utils {

class PipelineUtils {
public:
    static VkViewport CreateViewport(float width,
                                     float height,
                                     float min_depth = 0.0f,
                                     float max_depth = 1.0f,
                                     float x = 0.0f,
                                     float y = 0.0f);

    static VkRect2D CreateScissor(uint32_t width, uint32_t height, int32_t offset_x = 0, int32_t offset_y = 0);

    static VkPipelineColorBlendAttachmentState CreateDefaultColorBlendAttachment();
    static VkPipelineColorBlendAttachmentState CreateAlphaBlendAttachment();
    static VkPipelineColorBlendAttachmentState CreateAdditiveBlendAttachment();
    static VkPipelineColorBlendAttachmentState CreateNoBlendAttachment();

    static VkPipelineDepthStencilStateCreateInfo CreateDefaultDepthStencilState();
    static VkPipelineDepthStencilStateCreateInfo CreateDepthTestOnlyState();
    static VkPipelineDepthStencilStateCreateInfo CreateNoDepthTestState();

    static VkPipelineRasterizationStateCreateInfo CreateDefaultRasterizationState();
    static VkPipelineRasterizationStateCreateInfo CreateWireframeRasterizationState();
    static VkPipelineRasterizationStateCreateInfo CreateNoCullRasterizationState();

    static VkPipelineMultisampleStateCreateInfo CreateDefaultMultisampleState();
    static VkPipelineMultisampleStateCreateInfo CreateMsaaMultisampleState(VkSampleCountFlagBits samples);

    static VkVertexInputBindingDescription CreateVertexInputBinding(uint32_t binding,
                                                                    uint32_t stride,
                                                                    VkVertexInputRate input_rate);

    static VkVertexInputAttributeDescription CreateVertexInputAttribute(uint32_t location,
                                                                        uint32_t binding,
                                                                        VkFormat format,
                                                                        uint32_t offset);

    static VkPipelineShaderStageCreateInfo CreateShaderStageCreateInfo(VkShaderStageFlagBits stage,
                                                                       VkShaderModule module,
                                                                       const char* entry_point = nullptr,
                                                                       const VkSpecializationInfo* specialization_info = nullptr);

    static VkPipelineInputAssemblyStateCreateInfo CreateInputAssemblyState(VkPrimitiveTopology topology,
                                                                           VkBool32 primitive_restart_enable = VK_FALSE);

    static VkPipelineTessellationStateCreateInfo CreateTessellationState(uint32_t patch_control_points = 0);

    static VkPipelineViewportStateCreateInfo CreateViewportState(const std::vector<VkViewport>& viewports,
                                                                 const std::vector<VkRect2D>& scissors);

    static VkPipelineViewportStateCreateInfo CreateDynamicViewportState(uint32_t viewport_count, uint32_t scissor_count);

    static VkPipelineColorBlendStateCreateInfo CreateColorBlendState(const std::vector<VkPipelineColorBlendAttachmentState>& attachments,
                                                                     VkBool32 logic_op_enable = VK_FALSE,
                                                                     VkLogicOp logic_op = VK_LOGIC_OP_CLEAR);

    static VkPipelineDynamicStateCreateInfo CreateDynamicState(const std::vector<VkDynamicState>& dynamic_states);

    static std::vector<VkDynamicState> GetBasicDynamicStates();
    static std::vector<VkDynamicState> GetExtendedDynamicStates();
};

} // namespace VulkanEngine::Utils
