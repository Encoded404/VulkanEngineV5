module;

#include <volk.h>
#include <cstdint>

export module VulkanEngine.Utils.PipelineUtils;

export namespace VulkanEngine::Utils {

class PipelineUtils {
public:
    // Placeholder declarations; real signatures copied from header
    static VkViewport CreateViewport(uint32_t width, uint32_t height);
    static VkRect2D CreateScissor(uint32_t width, uint32_t height);
};

} // namespace VulkanEngine::Utils
