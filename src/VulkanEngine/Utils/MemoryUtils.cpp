#include <volk.h>
import VulkanEngine.Utils.MemoryUtils;

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>


namespace VulkanEngine::Utils {

namespace {
std::optional<uint32_t> FindMemoryTypeInternal(VkPhysicalDevice physical_device,
                                               uint32_t type_filter,
                                               VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);

    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (mem_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    return std::nullopt;
}

std::optional<uint32_t> TryFindMemoryType(VkPhysicalDevice physical_device,
                                          uint32_t type_filter,
                                          VkMemoryPropertyFlags required,
                                          VkMemoryPropertyFlags preferred) {
    if (auto index = FindMemoryTypeInternal(physical_device, type_filter, required | preferred)) {
        return index;
    }
    if (preferred != 0) {
        if (auto index = FindMemoryTypeInternal(physical_device, type_filter, required | preferred)) {
            return index;
        }
    }
    if (required != 0) {
        if (auto index = FindMemoryTypeInternal(physical_device, type_filter, required)) {
            return index;
        }
    }
    return std::nullopt;
}

} // namespace

uint32_t MemoryUtils::FindMemoryType(VkPhysicalDevice physical_device,
                                     uint32_t type_filter,
                                     VkMemoryPropertyFlags properties) {
    if (auto index = FindMemoryTypeInternal(physical_device, type_filter, properties)) {
        return *index;
    }
    throw std::runtime_error("Failed to find suitable memory type");
}

bool MemoryUtils::GetMemoryBudget(VkPhysicalDevice physical_device,
                                  std::vector<VkDeviceSize>& heap_budgets,
                                  std::vector<VkDeviceSize>& heap_usages) {
    auto vk_get_physical_device_memory_properties2_ptr = vkGetPhysicalDeviceMemoryProperties2;
    if (!vk_get_physical_device_memory_properties2_ptr) {
        return false;
    }

    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget_props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    properties.pNext = &budget_props;

    vk_get_physical_device_memory_properties2_ptr(physical_device, &properties);

    const uint32_t heap_count = properties.memoryProperties.memoryHeapCount;
    heap_budgets.assign(budget_props.heapBudget, budget_props.heapBudget + heap_count);
    heap_usages.assign(budget_props.heapUsage, budget_props.heapUsage + heap_count);

    const bool has_non_zero_budget = std::ranges::any_of(heap_budgets, [](VkDeviceSize value) {
        return value != 0;
    });

    const bool has_usage_data = std::ranges::any_of(heap_usages, [](VkDeviceSize value) {
        return value != 0;
    });

    return has_non_zero_budget || has_usage_data;
}

VkDeviceSize MemoryUtils::AlignedSize(VkDeviceSize size, VkDeviceSize alignment) {
    if (alignment == 0) {
        return size;
    }
    const VkDeviceSize remainder = size % alignment;
    if (remainder == 0) {
        return size;
    }
    return size + alignment - remainder;
}

VkMemoryPropertyFlags MemoryUtils::GetMemoryTypeProperties(VkPhysicalDevice physical_device,
                                                          uint32_t memory_type_index) {
    VkPhysicalDeviceMemoryProperties mem_properties{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_properties);
    if (memory_type_index >= mem_properties.memoryTypeCount) {
        throw std::out_of_range("Memory type index out of range");
    }
    return mem_properties.memoryTypes[memory_type_index].propertyFlags;
}

bool MemoryUtils::IsMemoryTypeHostVisible(VkPhysicalDevice physical_device, uint32_t memory_type_index) {
    const VkMemoryPropertyFlags flags = GetMemoryTypeProperties(physical_device, memory_type_index);
    return (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
}

bool MemoryUtils::IsMemoryTypeDeviceLocal(VkPhysicalDevice physical_device, uint32_t memory_type_index) {
    const VkMemoryPropertyFlags flags = GetMemoryTypeProperties(physical_device, memory_type_index);
    return (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
}

bool MemoryUtils::IsMemoryTypeHostCoherent(VkPhysicalDevice physical_device, uint32_t memory_type_index) {
    const VkMemoryPropertyFlags flags = GetMemoryTypeProperties(physical_device, memory_type_index);
    return (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
}

uint32_t MemoryUtils::GetOptimalBufferMemoryType(VkPhysicalDevice physical_device,
                                                 VkDevice device,
                                                 VkBuffer buffer,
                                                 VkMemoryPropertyFlags preferred_properties,
                                                 VkMemoryPropertyFlags required_properties) {
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);

    if (auto index = TryFindMemoryType(physical_device,
                                       requirements.memoryTypeBits,
                                       required_properties,
                                       preferred_properties)) {
        return *index;
    }

    throw std::runtime_error("Failed to find suitable memory type for buffer");
}

uint32_t MemoryUtils::GetOptimalImageMemoryType(VkPhysicalDevice physical_device,
                                                VkDevice device,
                                                VkImage image,
                                                VkMemoryPropertyFlags preferred_properties,
                                                VkMemoryPropertyFlags required_properties) {
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);

    if (auto index = TryFindMemoryType(physical_device,
                                       requirements.memoryTypeBits,
                                       required_properties,
                                       preferred_properties)) {
        return *index;
    }

    throw std::runtime_error("Failed to find suitable memory type for image");
}

VkDeviceSize MemoryUtils::CalculateAlignedBufferSize(VkDeviceSize size, VkDeviceSize min_alignment) {
    return AlignedSize(size, min_alignment);
}

VkDeviceSize MemoryUtils::GetNonCoherentAtomSize(VkPhysicalDevice physical_device) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    return properties.limits.nonCoherentAtomSize;
}

} // namespace VulkanEngine::Utils
