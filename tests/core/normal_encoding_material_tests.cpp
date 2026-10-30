#include <gtest/gtest.h>

import std;

import VulkanEngine.TextureTypes;
import VulkanEngine.TechniqueManager.DefaultMeshTechnique;

namespace {

using VulkanEngine::TechniqueManager::kNormalEncodingMask;
using VulkanEngine::TechniqueManager::PackNormalEncoding;
using VulkanEngine::Textures::TextureNormalEncoding;

// The material's flags bits 0..1 carry the declared normal encoding, and the
// shader decodes by comparing those bits to the packed values. The only
// properties the C++ side must guarantee are structural: the pack is a stable
// injection over the declared encodings that never sets bits outside the
// encoding field. The concrete ordinals are the shader's business and are not
// pinned here, except where a contract crosses the language boundary and
// cannot be derived from C++ (see DefaultFlagsReadAsStandard below).

// Two encodings that share a masked field would decode as the same encoding no
// matter what the caller intended, so the pack must be injective over the
// declared encodings -- under any combination of the other flag bits a caller
// may have OR-ed in.
TEST(NormalEncodingMaterialTest, PackIsInjectiveForEveryFlagCombination) {
    const std::array encodings = {TextureNormalEncoding::Standard, TextureNormalEncoding::Full,
                                  TextureNormalEncoding::Bent};
    const std::array<std::uint32_t, 4> other_bits = {
        0u,
        VulkanEngine::TechniqueManager::kAlphaMaskFlag,
        VulkanEngine::TechniqueManager::kHasTransformFlag,
        VulkanEngine::TechniqueManager::kAlphaMaskFlag |
            VulkanEngine::TechniqueManager::kHasTransformFlag,
    };

    for (const std::uint32_t extra : other_bits) {
        for (std::size_t i = 0; i < encodings.size(); ++i) {
            for (std::size_t j = i + 1; j < encodings.size(); ++j) {
                EXPECT_NE((PackNormalEncoding(encodings[i]) | extra) & kNormalEncodingMask,
                          (PackNormalEncoding(encodings[j]) | extra) & kNormalEncodingMask)
                    << "encoding [" << i << "] and [" << j << "] collide under flags "
                    << extra;
            }
        }
    }
}

TEST(NormalEncodingMaterialTest, DefaultFlagsReadAsStandard) {
    // Irreducible cross-language contract: the shader's first decode branch
    // treats a zero flags field as Standard, and nothing on the C++ side can
    // derive that, so it stays pinned as an explicit assertion.
    VulkanEngine::TechniqueManager::DefaultMeshPerMaterialData material{};
    EXPECT_EQ(material.flags & kNormalEncodingMask, PackNormalEncoding(TextureNormalEncoding::Standard));
}

}  // namespace
