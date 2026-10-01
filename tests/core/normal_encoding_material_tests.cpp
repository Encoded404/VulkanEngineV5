#include <gtest/gtest.h>

import std;

import VulkanEngine.TextureTypes;
import VulkanEngine.TechniqueManager.DefaultMeshTechnique;

namespace {

using VulkanEngine::TechniqueManager::kNormalEncodingMask;
using VulkanEngine::TechniqueManager::PackNormalEncoding;
using VulkanEngine::Textures::TextureNormalEncoding;

// The material's flags bits 0..1 carry the declared normal encoding, and the
// shader decodes by comparing those bits to 0 (Standard) / 1 (Full). The
// packing must therefore be the plain enum value, masked so reserved bits can
// never be set by an authoring call site.

TEST(NormalEncodingMaterialTest, PacksEachEncodingIntoBitsZeroAndOne) {
    EXPECT_EQ(PackNormalEncoding(TextureNormalEncoding::Standard), 0u);
    EXPECT_EQ(PackNormalEncoding(TextureNormalEncoding::Full), 1u);
    EXPECT_EQ(PackNormalEncoding(TextureNormalEncoding::Bent), 2u);
}

TEST(NormalEncodingMaterialTest, NeverSetsReservedBits) {
    for (const auto encoding : {TextureNormalEncoding::Standard, TextureNormalEncoding::Full,
                                TextureNormalEncoding::Bent}) {
        EXPECT_EQ(PackNormalEncoding(encoding) & ~kNormalEncodingMask, 0u);
    }
}

TEST(NormalEncodingMaterialTest, DefaultFlagsReadAsStandard) {
    // A zero flags field must mean Standard: the shader's first branch.
    VulkanEngine::TechniqueManager::DefaultMeshPerMaterialData material{};
    EXPECT_EQ(material.flags & kNormalEncodingMask, PackNormalEncoding(TextureNormalEncoding::Standard));
}

TEST(NormalEncodingMaterialTest, CombinesWithOtherFlagBits) {
    // The encoding occupies bits 0..1; callers OR in the alpha/has-transform
    // bits, so the packing must leave those untouched.
    const std::uint32_t flags = PackNormalEncoding(TextureNormalEncoding::Bent) |
                                VulkanEngine::TechniqueManager::kAlphaMaskFlag |
                                VulkanEngine::TechniqueManager::kHasTransformFlag;
    EXPECT_EQ(flags & kNormalEncodingMask, 2u);
    EXPECT_NE(flags & VulkanEngine::TechniqueManager::kAlphaMaskFlag, 0u);
    EXPECT_NE(flags & VulkanEngine::TechniqueManager::kHasTransformFlag, 0u);
}

}  // namespace
