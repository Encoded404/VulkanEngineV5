module;

export module VulkanBackend.Vulkan.VulkanCapabilities;

import std;

import vulkan_hpp;

export namespace VulkanBackend::Vulkan {

// ── Requirement levels ────────────────────────────────────────────────

enum class Requirement : std::uint8_t {
    Required,
    Optional,
};

// ── Instance-level extensions (loader/driver-global) ──────────────────

enum class InstanceExtension : std::uint16_t {
    DebugUtils,   // VK_EXT_debug_utils
    Count,
};

// ── Device-level extensions (per physical device) ─────────────────────

enum class DeviceExtension : std::uint16_t {
    Swapchain,              // VK_KHR_swapchain                  (Required)
    DebugUtils,             // VK_EXT_debug_utils                (Optional, linked to instance entry)
    PipelineLibrary,        // VK_KHR_pipeline_library           (Optional, auto with GPL)
    GraphicsPipelineLibrary, // VK_EXT_graphics_pipeline_library (Optional)
    Count,
};

// ── Device features (per physical device) ─────────────────────────────

enum class Feature : std::uint16_t {
    // core 1.0
    PipelineStatisticsQuery,
    // core 1.1
    ShaderDrawParameters,
    // core 1.2
    HostQueryReset,
    DescriptorIndexing,
    ShaderSampledImageArrayNonUniformIndexing,
    ShaderStorageImageArrayNonUniformIndexing,
    ShaderStorageBufferArrayNonUniformIndexing,
    RuntimeDescriptorArray,
    DescriptorBindingPartiallyBound,
    DescriptorBindingSampledImageUpdateAfterBind,
    DescriptorBindingStorageBufferUpdateAfterBind,
    DescriptorBindingVariableDescriptorCount,
    ScalarBlockLayout,  // scalar block layout for StructuredBuffers (Slang CDataLayout/ScalarDataLayout)
    // core 1.3
    DynamicRendering,
    PipelineCreationCacheControl,
    // EXT
    GraphicsPipelineLibrary,
    Count,
};

// ── Catalog spec structs ──────────────────────────────────────────────

struct InstanceExtensionSpec {
    std::string_view name; // NOLINT(misc-non-private-member-variables-in-classes)
    Requirement requirement; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct DeviceExtensionSpec {
    std::string_view name; // NOLINT(misc-non-private-member-variables-in-classes)
    Requirement requirement; // NOLINT(misc-non-private-member-variables-in-classes)
    std::span<const DeviceExtension> dependencies{};   // auto-enabled transitively; NOLINT(misc-non-private-member-variables-in-classes)
    std::optional<Feature> gate{};                     // feature that must also be enabled; NOLINT(misc-non-private-member-variables-in-classes)
};

struct FeatureSpec {
    std::string_view name; // NOLINT(misc-non-private-member-variables-in-classes)
    Requirement requirement; // NOLINT(misc-non-private-member-variables-in-classes)
};

// ── Catalogs ──────────────────────────────────────────────────────────

inline constexpr std::array<InstanceExtensionSpec, static_cast<std::size_t>(InstanceExtension::Count)> kInstanceExtensionCatalog = {{
    { "VK_EXT_debug_utils", Requirement::Optional },              // InstanceExtension::DebugUtils
}};

// std::span cannot be brace-initialized from a bare enum list; use a named array.
inline constexpr std::array<DeviceExtension, 1> kGplDependencies{ DeviceExtension::PipelineLibrary };

inline constexpr std::array<DeviceExtensionSpec, static_cast<std::size_t>(DeviceExtension::Count)> kDeviceExtensionCatalog = {{
    { "VK_KHR_swapchain",                 Requirement::Required, {}, {} },                                   // Swapchain
    { "VK_EXT_debug_utils",               Requirement::Optional, {}, {} },                                   // DebugUtils
    { "VK_KHR_pipeline_library",          Requirement::Optional, {}, {} },                                   // PipelineLibrary
    { "VK_EXT_graphics_pipeline_library", Requirement::Optional,
      std::span<const DeviceExtension>(kGplDependencies), Feature::GraphicsPipelineLibrary },                // GraphicsPipelineLibrary
}};

inline constexpr std::array<FeatureSpec, static_cast<std::size_t>(Feature::Count)> kFeatureCatalog = {{
    { "pipelineStatisticsQuery", Requirement::Required },
    { "shaderDrawParameters", Requirement::Required },
    { "hostQueryReset", Requirement::Required },
    { "descriptorIndexing", Requirement::Required },
    { "shaderSampledImageArrayNonUniformIndexing", Requirement::Required },
    { "shaderStorageImageArrayNonUniformIndexing", Requirement::Required },
    { "shaderStorageBufferArrayNonUniformIndexing", Requirement::Required },
    { "runtimeDescriptorArray", Requirement::Required },
    { "descriptorBindingPartiallyBound", Requirement::Required },
    { "descriptorBindingSampledImageUpdateAfterBind", Requirement::Required },
    { "descriptorBindingStorageBufferUpdateAfterBind", Requirement::Required },
    { "descriptorBindingVariableDescriptorCount", Requirement::Required },
    { "scalarBlockLayout", Requirement::Required },
    { "dynamicRendering", Requirement::Required },
    { "pipelineCreationCacheControl", Requirement::Required },
    { "graphicsPipelineLibrary", Requirement::Optional },
}};

// ── SupportedDeviceState: raw one-time query results (device) ─────────

struct SupportedDeviceState {
    vk::PhysicalDeviceProperties properties{}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::PhysicalDeviceDescriptorIndexingProperties descriptor_indexing{}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::PhysicalDeviceDriverProperties driver_properties{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<vk::ExtensionProperties> extensions; // NOLINT(misc-non-private-member-variables-in-classes)

    // Features2 query chain: features2.pNext → vulkan11 → vulkan12 → vulkan13
    // → (only if VK_EXT_graphics_pipeline_library was enumerated) gpl
    vk::PhysicalDeviceFeatures2 features2{}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::PhysicalDeviceVulkan11Features vulkan11{}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::PhysicalDeviceVulkan12Features vulkan12{}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::PhysicalDeviceVulkan13Features vulkan13{}; // NOLINT(misc-non-private-member-variables-in-classes)
    vk::PhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{}; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool HasExtension(std::string_view name) const noexcept {
        for (const auto& ext : extensions) {
            if (std::string_view(ext.extensionName) == name) {
                return true;
            }
        }
        return false;
    }
};

// ── VulkanInstanceCapabilities snapshot (instance/loader domain) ──────

class VulkanInstanceCapabilitiesBuilder;

class VulkanInstanceCapabilities {
public:
    // ── Queries (noexcept, O(1) for IDs, immutable after instance creation) ──
    [[nodiscard]] bool IsInstanceExtensionEnabled(InstanceExtension e) const noexcept {
        return exts_[static_cast<std::size_t>(e)];
    }
    [[nodiscard]] bool IsInstanceExtensionEnabled(std::string_view name) const noexcept {
        for (const auto& enabled : enabled_ext_names_) {
            if (enabled == name) return true;
        }
        return false;
    }
    [[nodiscard]] const std::string& GetErrorMessage() const noexcept { return error_message_; }
    [[nodiscard]] bool HasUnmetRequirements() const noexcept { return requirements_unmet_; }

private:
    friend class VulkanInstanceCapabilitiesBuilder;
    friend class VulkanCapabilities;   // for Summarize()

    std::bitset<static_cast<std::size_t>(InstanceExtension::Count)> exts_{};
    std::unordered_set<std::string> enabled_ext_names_{};
    std::string error_message_{};
    bool requirements_unmet_ = false;
};

class VulkanInstanceCapabilitiesBuilder {
public:
    explicit VulkanInstanceCapabilitiesBuilder(VulkanInstanceCapabilities& caps) : caps_(caps) {}

    void SetExtensionEnabled(InstanceExtension e) {
        caps_.exts_.set(static_cast<std::size_t>(e));
    }
    void AddExtensionName(std::string_view name) {
        caps_.enabled_ext_names_.emplace(name);
    }
    void AppendError(std::string_view message) {
        if (!caps_.error_message_.empty()) caps_.error_message_ += "; ";
        caps_.error_message_ += message;
    }
    void SetRequirementsUnmet() {
        caps_.requirements_unmet_ = true;
    }

private:
    VulkanInstanceCapabilities& caps_;
};

// ── VulkanCapabilities snapshot (physical-device domain) ──────────────

class VulkanCapabilitiesBuilder;

class VulkanCapabilities {
public:
    // ── Queries (noexcept, O(1), immutable after device creation) ──
    [[nodiscard]] bool IsDeviceExtensionEnabled(DeviceExtension e) const noexcept {
        return device_exts_[static_cast<std::size_t>(e)];
    }
    [[nodiscard]] bool IsFeatureEnabled(Feature f) const noexcept {
        return features_[static_cast<std::size_t>(f)];
    }
    [[nodiscard]] bool CanUse(DeviceExtension e) const noexcept {
        if (!IsDeviceExtensionEnabled(e)) return false;
        if (const auto gate = kDeviceExtensionCatalog[static_cast<std::size_t>(e)].gate) {
            return IsFeatureEnabled(*gate);
        }
        return true;
    }
    [[nodiscard]] const vk::PhysicalDeviceProperties& GetProperties() const noexcept { return properties_; }
    [[nodiscard]] const vk::PhysicalDeviceDescriptorIndexingProperties& GetDescriptorIndexingProperties() const noexcept {
        return descriptor_indexing_;
    }
    [[nodiscard]] const vk::PhysicalDeviceDriverProperties& GetDriverProperties() const noexcept { return driver_properties_; }
    [[nodiscard]] const std::string& GetErrorMessage() const noexcept { return error_message_; }
    [[nodiscard]] bool HasUnmetRequirements() const noexcept { return requirements_unmet_; }
    [[nodiscard]] std::string Summarize(const VulkanInstanceCapabilities& instance_caps) const;

private:
    friend class VulkanCapabilitiesBuilder;

    std::bitset<static_cast<std::size_t>(DeviceExtension::Count)> device_exts_{};
    std::bitset<static_cast<std::size_t>(Feature::Count)> features_{};
    std::unordered_set<std::string> enabled_device_ext_names_{};   // dynamic app device extensions

    // Feature chain structs — the EXACT structs chained into vkCreateDevice
    // (stable addresses, owned here for lifetime).
    vk::PhysicalDeviceFeatures2 core_features2_{};
    vk::PhysicalDeviceVulkan11Features vulkan11_features_{};
    vk::PhysicalDeviceVulkan12Features vulkan12_features_{};
    vk::PhysicalDeviceVulkan13Features vulkan13_features_{};
    vk::PhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl_features_{};

    vk::PhysicalDeviceProperties properties_{};
    vk::PhysicalDeviceDescriptorIndexingProperties descriptor_indexing_{};
    vk::PhysicalDeviceDriverProperties driver_properties_{};

    std::string error_message_{};
    bool requirements_unmet_ = false;
};

class VulkanCapabilitiesBuilder {
public:
    VulkanCapabilitiesBuilder(VulkanCapabilities& caps, const SupportedDeviceState& supported)
        : caps_(caps), supported_(supported) {}

    void SetDeviceExtensions(std::span<const DeviceExtension> exts) {
        for (const auto e : exts) {
            caps_.device_exts_.set(static_cast<std::size_t>(e));
        }
    }
    void AddDeviceExtensionName(std::string_view name) {
        caps_.enabled_device_ext_names_.emplace(name);
    }
    void SetProperties(const vk::PhysicalDeviceProperties& properties) {
        caps_.properties_ = properties;
    }
    void SetDescriptorIndexingProperties(const vk::PhysicalDeviceDescriptorIndexingProperties& properties) {
        caps_.descriptor_indexing_ = properties;
    }
    void SetDriverProperties(const vk::PhysicalDeviceDriverProperties& properties) {
        caps_.driver_properties_ = properties;
    }
    void AppendError(std::string_view message) {
        if (!caps_.error_message_.empty()) caps_.error_message_ += "; ";
        caps_.error_message_ += message;
    }
    void SetRequirementsUnmet() {
        caps_.requirements_unmet_ = true;
    }

    [[nodiscard]] bool IsFeatureSupported(Feature f) const noexcept;
    void SetFeatureRequest(Feature f, bool required);
    void FinalizeFeatures();

    // Root of the create-time feature chain (chained into VkDeviceCreateInfo).
    [[nodiscard]] const vk::PhysicalDeviceFeatures2& GetFeatureChain() const noexcept {
        return caps_.core_features2_;
    }

private:
    static bool GetSupportedFeature(const SupportedDeviceState& supported, Feature f) noexcept;
    static bool GetRequestedFeature(const VulkanCapabilities& caps, Feature f) noexcept;
    static void SetRequestedFeature(VulkanCapabilities& caps, Feature f, bool value) noexcept;

    VulkanCapabilities& caps_;
    const SupportedDeviceState& supported_;
};

} // namespace VulkanBackend::Vulkan
