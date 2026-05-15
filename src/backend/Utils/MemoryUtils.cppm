module;

#include <vulkan/vulkan_raii.hpp>
#include <vector>

export module VulkanBackend.Utils.MemoryUtils;

export namespace VulkanEngine::Utils {

class MemoryUtils {
public:
    static uint32_t FindMemoryType(vk::PhysicalDevice physical_device,
                                  uint32_t type_filter,
                                  vk::MemoryPropertyFlags properties);

    static bool GetMemoryBudget(vk::PhysicalDevice physical_device,
                               std::vector<vk::DeviceSize>& heap_budgets,
                               std::vector<vk::DeviceSize>& heap_usages);

    static vk::DeviceSize AlignedSize(vk::DeviceSize size, vk::DeviceSize alignment);

    static vk::MemoryPropertyFlags GetMemoryTypeProperties(vk::PhysicalDevice physical_device,
                                                         uint32_t memory_type_index);

    static bool IsMemoryTypeHostVisible(vk::PhysicalDevice physical_device, uint32_t memory_type_index);
    static bool IsMemoryTypeDeviceLocal(vk::PhysicalDevice physical_device, uint32_t memory_type_index);
    static bool IsMemoryTypeHostCoherent(vk::PhysicalDevice physical_device, uint32_t memory_type_index);

    static uint32_t GetOptimalBufferMemoryType(vk::PhysicalDevice physical_device,
                                              vk::Device device,
                                              vk::Buffer buffer,
                                              vk::MemoryPropertyFlags preferred_properties,
                                              vk::MemoryPropertyFlags required_properties = vk::MemoryPropertyFlags{});

    static uint32_t GetOptimalImageMemoryType(vk::PhysicalDevice physical_device,
                                             vk::Device device,
                                             vk::Image image,
                                             vk::MemoryPropertyFlags preferred_properties,
                                             vk::MemoryPropertyFlags required_properties = vk::MemoryPropertyFlags{});

    static vk::DeviceSize CalculateAlignedBufferSize(vk::DeviceSize size, vk::DeviceSize min_alignment);
    static vk::DeviceSize GetNonCoherentAtomSize(vk::PhysicalDevice physical_device);
};

} // namespace VulkanEngine::Utils
