#include <gtest/gtest.h>

import std;

import vulkan_hpp;
import VulkanBackend.Vulkan.VulkanCapabilities;

namespace {

using VulkanBackend::Vulkan::Feature;
using VulkanBackend::Vulkan::Requirement;
using VulkanBackend::Vulkan::SupportedDeviceState;
using VulkanBackend::Vulkan::VulkanCapabilities;
using VulkanBackend::Vulkan::VulkanCapabilitiesBuilder;
namespace Caps = VulkanBackend::Vulkan;

// MID requires both drawIndirectCount (core 1.2) and drawIndirectFirstInstance
// (core 1.0). The latter is catalogued Optional and mapped to the core
// VkPhysicalDeviceFeatures bit, so these tests are device-free: they drive the
// builder with a synthetic SupportedDeviceState.

TEST(VulkanCapabilitiesDrawIndirectFirstInstance, OptionalCatalogEntry) {
    const auto& spec = Caps::kFeatureCatalog[
        static_cast<std::size_t>(Feature::DrawIndirectFirstInstance)];
    EXPECT_EQ(spec.name, std::string_view("drawIndirectFirstInstance"));
    EXPECT_TRUE(spec.requirement == Requirement::Optional);
}

TEST(VulkanCapabilitiesDrawIndirectFirstInstance, SupportedMapsToCoreFeatureBit) {
    VulkanCapabilities caps;
    SupportedDeviceState supported{};
    supported.features2.features.drawIndirectFirstInstance = vk::True;

    VulkanCapabilitiesBuilder builder(caps, supported);
    builder.SetFeatureRequest(Feature::DrawIndirectFirstInstance, /*required=*/false);
    builder.FinalizeFeatures();

    EXPECT_TRUE(caps.IsFeatureEnabled(Feature::DrawIndirectFirstInstance));
    EXPECT_FALSE(caps.HasUnmetRequirements());
    EXPECT_EQ(builder.GetFeatureChain().features.drawIndirectFirstInstance, vk::True);
}

TEST(VulkanCapabilitiesDrawIndirectFirstInstance, UnsupportedOptionalStaysOff) {
    VulkanCapabilities caps;
    SupportedDeviceState supported{};
    supported.features2.features.drawIndirectFirstInstance = vk::False;

    VulkanCapabilitiesBuilder builder(caps, supported);
    builder.SetFeatureRequest(Feature::DrawIndirectFirstInstance, /*required=*/false);
    builder.FinalizeFeatures();

    EXPECT_FALSE(caps.IsFeatureEnabled(Feature::DrawIndirectFirstInstance));
    EXPECT_FALSE(caps.HasUnmetRequirements());
}

}  // namespace
