module;

export module VulkanBackend.Utils.VulkanDebugUtils;

import std;
import std.compat;

import vulkan_hpp;

export namespace VulkanEngine::Utils {

// RAII handles (vk::raii::*) — objectType is a static member.
// *obj returns the non-RAII C++ wrapper; static_cast<CType> invokes
// its implicit conversion to the raw Vk* handle.
template<typename HandleType>
void SetVulkanObjectName(const vk::raii::Device& dev, const HandleType& obj, const std::string& name) {
    vk::DebugUtilsObjectNameInfoEXT info{};
    info.sType = vk::StructureType::eDebugUtilsObjectNameInfoEXT;
    info.objectType = static_cast<vk::ObjectType>(HandleType::objectType);
    info.objectHandle = reinterpret_cast<std::uint64_t>(static_cast<typename HandleType::CType>(*obj));
    info.pObjectName = name.c_str();
    dev.setDebugUtilsObjectNameEXT(info);
}

// Non-RAII handles — explicit type.
template<typename HandleType>
void SetVulkanObjectName(const vk::raii::Device& dev, const HandleType& obj, vk::ObjectType type, const std::string& name) {
    auto* fn = dev.getDispatcher()->vkSetDebugUtilsObjectNameEXT;
    if (!fn) return;

    vk::DebugUtilsObjectNameInfoEXT info{};
    info.sType = vk::StructureType::eDebugUtilsObjectNameInfoEXT;
    info.objectType = type;
    if constexpr (std::is_pointer_v<HandleType>) {
        info.objectHandle = reinterpret_cast<std::uint64_t>(obj);
    } else {
        info.objectHandle = reinterpret_cast<std::uint64_t>(static_cast<typename HandleType::CType>(*obj));
    }
    info.pObjectName = name.c_str();
    dev.setDebugUtilsObjectNameEXT(info);
}

}
