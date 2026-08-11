#include <gtest/gtest.h>

import std;
import std.compat;

import VulkanEngine.GplPolicy;

namespace {

using namespace VulkanEngine::ShaderSystem;

// Mesa encodes driverVersion = VK_MAKE_VERSION(major, minor, patch)
// (src/vulkan/util/vk_util.c): bits 31..22 / 21..12 / 11..0 (doc §11).
// 25.3.6 = (25<<22)|(3<<12)|6 = 0x6403006 (doc §11's hex example has a stray
// zero: 0x64003006 decodes to (400, 48, 6), not (25, 3, 6)).
TEST(GplResolutionTest, DecodesMesa2536Version) {
    const auto v = DecodeDriverVersion(0x6403006u);
    EXPECT_EQ(v.major, 25u);
    EXPECT_EQ(v.minor, 3u);
    EXPECT_EQ(v.patch, 6u);
}

TEST(GplResolutionTest, DecodesMesa2606Version) {
    const auto v = DecodeDriverVersion(0x6800006u);
    EXPECT_EQ(v.major, 26u);
    EXPECT_EQ(v.minor, 0u);
    EXPECT_EQ(v.patch, 6u);
}

TEST(GplResolutionTest, DecodesMesaDevel99Encoding) {
    // 26.0.0-devel encodes as 25.99.99 (Mesa devel-99 convention, vk_util.c).
    const auto v = DecodeDriverVersion(0x6463063u);
    EXPECT_EQ(v.major, 25u);
    EXPECT_EQ(v.minor, 99u);
    EXPECT_EQ(v.patch, 99u);
}

TEST(GplResolutionTest, DecodesZeroAsUnknownVersion) {
    const auto v = DecodeDriverVersion(0u);
    EXPECT_EQ(v.major, 0u);
    EXPECT_EQ(v.minor, 0u);
    EXPECT_EQ(v.patch, 0u);
}

TEST(GplResolutionTest, IsAffectedRadvMatrix) {
    constexpr std::uint32_t kRadv = 3u;      // VK_DRIVER_ID_MESA_RADV
    constexpr std::uint32_t kMesa2536 = 0x6403006u;
    constexpr std::uint32_t kMesa2606 = 0x6800006u;
    constexpr std::uint32_t kMesaDevel = 0x6463063u;  // 25.99.99

    EXPECT_TRUE(IsAffectedRadv(kRadv, kMesa2536));
    EXPECT_FALSE(IsAffectedRadv(kRadv, kMesa2606));
    // Devel-99 encoding classifies as affected — the safe direction.
    EXPECT_TRUE(IsAffectedRadv(kRadv, kMesaDevel));
    // Unknown/zero version → conservative (affected).
    EXPECT_TRUE(IsAffectedRadv(kRadv, 0u));
    // Non-RADV driver IDs are never affected.
    EXPECT_FALSE(IsAffectedRadv(6u, kMesa2536));   // ANV
    EXPECT_FALSE(IsAffectedRadv(13u, kMesa2536));  // lavapipe
}

TEST(GplResolutionTest, ResolveGplMatrix) {
    constexpr std::uint32_t kRadv = 3u;
    constexpr std::uint32_t kRadvAffected = 0x6403006u;  // 25.3.6
    constexpr std::uint32_t kRadvFixed = 0x6800006u;      // 26.0.6
    constexpr std::uint32_t kNonRadv = 13u;               // lavapipe

    // auto/auto on RADV-affected → split + use
    {
        const auto r = ResolveGpl(GplPolicy::Auto, GplStructurePolicy::Auto,
                                  true, kRadv, kRadvAffected);
        EXPECT_TRUE(r.use_gpl);
        EXPECT_EQ(r.structure, GplStructurePolicy::Split);
        EXPECT_EQ(r.warning, GplResolution::Warning::None);
        EXPECT_TRUE(r.affected_radv);
    }
    // auto/auto on RADV-fixed → combined + use
    {
        const auto r = ResolveGpl(GplPolicy::Auto, GplStructurePolicy::Auto,
                                  true, kRadv, kRadvFixed);
        EXPECT_TRUE(r.use_gpl);
        EXPECT_EQ(r.structure, GplStructurePolicy::Combined);
        EXPECT_EQ(r.warning, GplResolution::Warning::None);
        EXPECT_FALSE(r.affected_radv);
    }
    // auto/auto on non-RADV → combined + use
    {
        const auto r = ResolveGpl(GplPolicy::Auto, GplStructurePolicy::Auto,
                                  true, kNonRadv, kRadvAffected);
        EXPECT_TRUE(r.use_gpl);
        EXPECT_EQ(r.structure, GplStructurePolicy::Combined);
        EXPECT_EQ(r.warning, GplResolution::Warning::None);
        EXPECT_FALSE(r.affected_radv);
    }
    // auto/split anywhere supported → split + use
    {
        const auto r = ResolveGpl(GplPolicy::Auto, GplStructurePolicy::Split,
                                  true, kNonRadv, kRadvFixed);
        EXPECT_TRUE(r.use_gpl);
        EXPECT_EQ(r.structure, GplStructurePolicy::Split);
        EXPECT_EQ(r.warning, GplResolution::Warning::None);
    }
    // auto/combined on affected → deny + CombinedDenied
    {
        const auto r = ResolveGpl(GplPolicy::Auto, GplStructurePolicy::Combined,
                                  true, kRadv, kRadvAffected);
        EXPECT_FALSE(r.use_gpl);
        EXPECT_EQ(r.warning, GplResolution::Warning::CombinedDenied);
    }
    // force/combined on affected → use + ForcedCombinedFootgun
    {
        const auto r = ResolveGpl(GplPolicy::Forced, GplStructurePolicy::Combined,
                                  true, kRadv, kRadvAffected);
        EXPECT_TRUE(r.use_gpl);
        EXPECT_EQ(r.structure, GplStructurePolicy::Combined);
        EXPECT_EQ(r.warning, GplResolution::Warning::ForcedCombinedFootgun);
    }
    // force/split on affected → split + use
    {
        const auto r = ResolveGpl(GplPolicy::Forced, GplStructurePolicy::Split,
                                  true, kRadv, kRadvAffected);
        EXPECT_TRUE(r.use_gpl);
        EXPECT_EQ(r.structure, GplStructurePolicy::Split);
        EXPECT_EQ(r.warning, GplResolution::Warning::None);
    }
    // force/auto on affected → split + use
    {
        const auto r = ResolveGpl(GplPolicy::Forced, GplStructurePolicy::Auto,
                                  true, kRadv, kRadvAffected);
        EXPECT_TRUE(r.use_gpl);
        EXPECT_EQ(r.structure, GplStructurePolicy::Split);
        EXPECT_EQ(r.warning, GplResolution::Warning::None);
    }
    // force + unsupported → deny + ForcedUnsupported
    {
        const auto r = ResolveGpl(GplPolicy::Forced, GplStructurePolicy::Auto,
                                  false, kRadv, kRadvAffected);
        EXPECT_FALSE(r.use_gpl);
        EXPECT_EQ(r.warning, GplResolution::Warning::ForcedUnsupported);
    }
    // auto + unsupported → deny, no warning
    {
        const auto r = ResolveGpl(GplPolicy::Auto, GplStructurePolicy::Auto,
                                  false, kRadv, kRadvAffected);
        EXPECT_FALSE(r.use_gpl);
        EXPECT_EQ(r.warning, GplResolution::Warning::None);
    }
    // disable → deny, no warning
    {
        const auto r = ResolveGpl(GplPolicy::Disable, GplStructurePolicy::Auto,
                                  true, kRadv, kRadvFixed);
        EXPECT_FALSE(r.use_gpl);
        EXPECT_EQ(r.warning, GplResolution::Warning::None);
    }
    // auto/combined on non-RADV → combined + use
    {
        const auto r = ResolveGpl(GplPolicy::Auto, GplStructurePolicy::Combined,
                                  true, kNonRadv, kRadvAffected);
        EXPECT_TRUE(r.use_gpl);
        EXPECT_EQ(r.structure, GplStructurePolicy::Combined);
        EXPECT_EQ(r.warning, GplResolution::Warning::None);
    }
}

}  // namespace
