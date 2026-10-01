module;

#include <vulkan/vulkan_core.h>

export module VulkanEngine.TextureTypes;

import std;

import vulkan_hpp;

export namespace VulkanEngine::Textures {

// ── Requirement-level enums ───────────────────────────────────────────

enum class TextureSemantic : std::uint8_t {
    BaseColor,
    Emissive,
    Normal,
    ORM,
    Mask,
    UI,
    HDR,
    Unknown,
};

// Declared at import/authoring time; NOT derived from the GPU format.
// Determines both the shader decode path and which upload formats are legal.
enum class TextureNormalEncoding : std::uint8_t {
    Standard = 0, // unit tangent-space normal; reconstruct z = sqrt(1 - x^2 - y^2) from RG.
    Full     = 1, // authored RGB normal, possibly non-unit or z < 0. Use RGB as-is.
    Bent     = 2, // stylized/bent direction; use RGB as-is and apply normal_scale to xy.
};

// Per-texture-slot KHR_texture_transform; stored here so
// the type exists once (a consumer reads it once UV authoring carries it).
struct TextureTransform {
    float offset_u{0.0f}, offset_v{0.0f};
    float scale_u{1.0f}, scale_v{1.0f};
    float rotation{0.0f};
};

// CPU-side sampler description, resolved to a VkSampler by the owning layer.
// No swizzle here: a component mapping is a view property (TextureData.swizzle
// applied through the image view), never a sampler property. Clamp to device
// limits before hashing/creating.
struct SamplerDesc {
    vk::Filter min_filter{vk::Filter::eLinear}, mag_filter{vk::Filter::eLinear};
    vk::SamplerMipmapMode mip_mode{vk::SamplerMipmapMode::eLinear};
    vk::SamplerAddressMode address_u{vk::SamplerAddressMode::eRepeat};
    vk::SamplerAddressMode address_v{vk::SamplerAddressMode::eRepeat};
    vk::SamplerAddressMode address_w{vk::SamplerAddressMode::eRepeat};
    float mip_lod_bias{0.0f};
    float min_lod{0.0f};
    float max_lod{1000.0f}; // VK_LOD_CLAMP_NONE
    bool anisotropy{false};
    std::uint32_t max_anisotropy{1};
    vk::CompareOp compare{vk::CompareOp::eNever};
    vk::BorderColor border{vk::BorderColor::eFloatOpaqueBlack};

    bool operator==(const SamplerDesc&) const = default;
};

// ── Alpha analysis (moved from the loader-local type; used by material
//    validation and by callers deciding fallbacks) ──────────────────────

struct AlphaAnalysis {
    bool hasAlphaChannel = false; //NOLINT(misc-non-private-member-variables-in-classes)
    bool hasFractionalAlpha = false; //NOLINT(misc-non-private-member-variables-in-classes)
    bool hasZeroAlpha = false; //NOLINT(misc-non-private-member-variables-in-classes)
    float opaqueCoverage = 1.0f; //NOLINT(misc-non-private-member-variables-in-classes)
};

// ── Subresource-complete texture payload ──────────────────────────────

// One entry per (mip, layer, face). gpu_layer = layer * face_count + face.
struct TextureSubresource {
    std::uint32_t mip{0};
    std::uint32_t array_layer{0};
    std::uint64_t offset{0}, size{0};       // into TextureData::blob, block-tight
    std::uint32_t width{0}, height{0}, depth{1};
};

// Canonical loader output: the loader-local struct of the same name is deleted;
// this is the single type produced by every texture loader. Loaders keep the
// container payload as the file provides it (supercompression included) and
// never inflate or transcode; the uploader/worker resolves the device format
// and performs inflate + transcode.
struct TextureData {
    std::uint32_t width{0}, height{0}, depth{1};
    std::uint32_t mip_levels{1}, array_layers{1}, face_count{1};
    vk::Format source_format{vk::Format::eUndefined};
    bool needs_transcode{false};       // KTX2 Basis (UASTC/ETC1S)
    bool is_hdr_source{false};
    std::uint8_t source_channels{0};   // from the DFD channel model, not the raw count
    std::uint8_t source_alpha{0};      // 0/1
    bool supercompressed{false};       // Zstd/ZLIB payload present in blob
    bool cube_complete{true};          // false when KTXcubemapIncomplete is set
    TextureSemantic semantic{TextureSemantic::Unknown};
    vk::ComponentMapping swizzle{};    // from KTXswizzle; applied to the view
    AlphaAnalysis alpha_analysis{};
    std::vector<TextureSubresource> subresources;
    // Container payload as the file provides it. For PNG/JPG loaders this is
    // decoded RGBA8 pixels (those containers have no supercompression).
    std::vector<std::byte> blob;

    void Reset() noexcept {
        width = 0;
        height = 0;
        depth = 1;
        mip_levels = 1;
        array_layers = 1;
        face_count = 1;
        source_format = vk::Format::eUndefined;
        needs_transcode = false;
        is_hdr_source = false;
        source_channels = 0;
        source_alpha = 0;
        supercompressed = false;
        cube_complete = true;
        semantic = TextureSemantic::Unknown;
        swizzle = vk::ComponentMapping{};
        alpha_analysis = AlphaAnalysis{};
        subresources.clear();
        blob.clear();
    }
};

} // namespace VulkanEngine::Textures
