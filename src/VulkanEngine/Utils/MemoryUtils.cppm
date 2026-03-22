#ifdef __has_cpp_attribute
#endif
module;

#include <volk.h>
#include <vector>
#include <cstdint>

export module VulkanEngine.Utils.MemoryUtils;

export namespace VulkanEngine::Utils {

class MemoryUtils {
public:
    static uint32_t FindMemoryType(VkPhysicalDevice physical_device,
                                  uint32_t type_filter,
                                  VkMemoryPropertyFlags properties);

    static bool GetMemoryBudget(VkPhysicalDevice physical_device,
                               std::vector<VkDeviceSize>& heap_budgets,
                               std::vector<VkDeviceSize>& heap_usages);

    static VkDeviceSize AlignedSize(VkDeviceSize size, VkDeviceSize alignment);

    static VkMemoryPropertyFlags GetMemoryTypeProperties(VkPhysicalDevice physical_device,
                                                         uint32_t memory_type_index);

    static bool IsMemoryTypeHostVisible(VkPhysicalDevice physical_device, uint32_t memory_type_index);
    static bool IsMemoryTypeDeviceLocal(VkPhysicalDevice physical_device, uint32_t memory_type_index);
    static bool IsMemoryTypeHostCoherent(VkPhysicalDevice physical_device, uint32_t memory_type_index);

    static uint32_t GetOptimalBufferMemoryType(VkPhysicalDevice physical_device,
                                              VkDevice device,
                                              VkBuffer buffer,
                                              VkMemoryPropertyFlags preferred_properties,
                                              VkMemoryPropertyFlags required_properties = 0);

    static uint32_t GetOptimalImageMemoryType(VkPhysicalDevice physical_device,
                                             VkDevice device,
                                             VkImage image,
                                             VkMemoryPropertyFlags preferred_properties,
                                             VkMemoryPropertyFlags required_properties = 0);

    static VkDeviceSize CalculateAlignedBufferSize(VkDeviceSize size, VkDeviceSize min_alignment);
    static VkDeviceSize GetNonCoherentAtomSize(VkPhysicalDevice physical_device);
};

} // namespace VulkanEngine::Utils
