#include <gtest/gtest.h>

import std;

import VulkanEngine.MaterialManager;
import VulkanEngine.SceneRenderer;
import VulkanEngine.ShaderManager;
import VulkanEngine.TechniqueManager;

namespace {

using VulkanEngine::MaterialManager::BlendMode;
using VulkanEngine::MaterialManager::DeriveRenderState;
using VulkanEngine::MaterialManager::MaterialDesc;
using VulkanEngine::MaterialManager::RenderStateKey;
using VulkanEngine::SceneRenderer::GroupWritesPrepassDepth;
using VulkanEngine::TechniqueManager::BaseTechnique;
using VulkanEngine::TechniqueManager::DepthCoverage;
using VulkanEngine::TechniqueManager::TechniqueManager;
using VulkanEngine::TechniqueManager::VariantShaderRow;
using MaterialRegistry = VulkanEngine::MaterialManager::MaterialManager;
namespace TechniquePacking = VulkanEngine::TechniqueManager::TechniquePacking;

// A minimal concrete technique so the manager can be exercised without a
// device. Registration never touches the GPU; it only assigns ids and interns
// the base draw group.
class TestTechnique final : public BaseTechnique {};

// A discarding technique with one PerMaterial binding, so the material
// registration cross-check can be exercised device-free. `UnlitTextureTechnique`
// itself is final, so the policy is selected through the constructor.
class MaskTechnique final : public BaseTechnique {
public:
    explicit MaskTechnique(DepthCoverage coverage) {
        DeclarePerMaterial<VulkanEngine::TechniqueManager::UnlitPerMaterialData>(5, 0);
        SetBaseDepthCoverage(coverage);
    }

    template<typename T>
    static constexpr bool HasBinding() {
        return std::is_same_v<T, VulkanEngine::TechniqueManager::UnlitPerMaterialData>;
    }
    template<typename T>
    static constexpr std::size_t GetOffset() {
        static_assert(HasBinding<T>());
        return 0;
    }
    template<typename T>
    static constexpr std::uint32_t GetBindingIndex() {
        static_assert(HasBinding<T>());
        return 0;
    }

    [[nodiscard]] VulkanEngine::TechniqueManager::AlphaMaskSupport AlphaMaskExpectation(
        std::span<const std::byte>) const override {
        return VulkanEngine::TechniqueManager::AlphaMaskSupport::Always;
    }
};

// ── Render state ──

TEST(DrawGroupTest, CutoutImpliesAlphaMask) {
    EXPECT_TRUE(DeriveRenderState(MaterialDesc{.blend = BlendMode::Cutout}).alpha_mask);
    EXPECT_FALSE(DeriveRenderState(MaterialDesc{.blend = BlendMode::Opaque}).alpha_mask);
    EXPECT_FALSE(DeriveRenderState(MaterialDesc{.blend = BlendMode::Transparent}).alpha_mask);
}

TEST(DrawGroupTest, RenderStateKeyDistinguishesBlendModes) {
    const MaterialDesc opaque{.blend = BlendMode::Opaque};
    const MaterialDesc transparent{.blend = BlendMode::Transparent};
    EXPECT_NE(RenderStateKey(opaque), RenderStateKey(transparent));
}

TEST(DrawGroupTest, RenderStateKeyDistinguishesDoubleSidedAndDepthWrite) {
    const MaterialDesc base{};
    MaterialDesc double_sided = base;
    double_sided.double_sided = true;
    MaterialDesc no_depth = base;
    no_depth.depth_write = false;
    EXPECT_NE(RenderStateKey(base), RenderStateKey(double_sided));
    EXPECT_NE(RenderStateKey(base), RenderStateKey(no_depth));
    EXPECT_NE(RenderStateKey(double_sided), RenderStateKey(no_depth));
}

TEST(DrawGroupTest, DefaultRenderStateKeyIsZero) {
    // The default descriptor must intern the seeded base group so existing
    // scenes keep group == technique id.
    EXPECT_EQ(RenderStateKey(MaterialDesc{}), 0u);
}

// ── Draw-group interning ──

TEST(DrawGroupTest, BaseGroupMatchesTechniqueId) {
    TechniqueManager mgr;
    const auto id = mgr.Register(std::make_unique<TestTechnique>());
    EXPECT_EQ(mgr.GetDrawGroupCount(), 1u);
    // The base group (variant 0, no render state) carries the technique id, so
    // the flag table and region table stay index-compatible with technique ids.
    EXPECT_EQ(mgr.GetDrawGroupTechnique(id.value), id.value);
    EXPECT_EQ(mgr.GetDrawGroupVariant(id.value), 0u);
}

TEST(DrawGroupTest, InterningIsDenseAndIdempotent) {
    TechniqueManager mgr;
    const auto a = mgr.Register(std::make_unique<TestTechnique>());
    const auto b = mgr.Register(std::make_unique<TestTechnique>());
    // Distinct techniques yield pairwise-distinct ids inside the dense range;
    // which id each technique lands on is not part of the contract.
    EXPECT_NE(a.value, b.value);
    EXPECT_LT(a.value, TechniquePacking::MAX_DRAW_GROUPS);
    EXPECT_LT(b.value, TechniquePacking::MAX_DRAW_GROUPS);
    EXPECT_EQ(mgr.GetDrawGroupCount(), 2u);

    // Interning a new triple claims some fresh dense group: the concrete id is
    // unspecified, only that it is in range, unclaimed, and owned by `a`.
    const std::uint16_t g = mgr.InternDrawGroup(a.value, 0, 0x40u);
    EXPECT_LT(g, TechniquePacking::MAX_DRAW_GROUPS);
    EXPECT_NE(g, a.value);
    EXPECT_NE(g, b.value);
    EXPECT_EQ(mgr.GetDrawGroupCount(), 3u);
    EXPECT_EQ(mgr.GetDrawGroupTechnique(g), a.value);

    // The same triple returns the same group.
    EXPECT_EQ(mgr.InternDrawGroup(a.value, 0, 0x40u), g);
    EXPECT_EQ(mgr.GetDrawGroupCount(), 3u);

    // A different technique's identical state is a different group.
    const std::uint16_t gb = mgr.InternDrawGroup(b.value, 0, 0x40u);
    EXPECT_NE(gb, g);
    EXPECT_LT(gb, TechniquePacking::MAX_DRAW_GROUPS);
    EXPECT_EQ(mgr.GetDrawGroupTechnique(gb), b.value);
}

TEST(DrawGroupTest, OutOfRangeGroupLookupsAreSafe) {
    TechniqueManager mgr;
    const auto id = mgr.Register(std::make_unique<TestTechnique>());
    (void)id;
    const std::uint16_t bogus = static_cast<std::uint16_t>(TechniquePacking::MAX_DRAW_GROUPS - 1);
    EXPECT_EQ(mgr.GetTechnique(mgr.GetDrawGroupTechnique(bogus)), nullptr);
}

TEST(DrawGroupTest, ShutdownResetsTheInternTable) {
    TechniqueManager mgr;
    (void)mgr.Register(std::make_unique<TestTechnique>());
    (void)mgr.InternDrawGroup(0, 0, 0x10u);
    EXPECT_GT(mgr.GetDrawGroupCount(), 0u);
    mgr.Shutdown();
    EXPECT_EQ(mgr.GetDrawGroupCount(), 0u);
    EXPECT_EQ(mgr.GetTechniqueCount(), 0u);
}

// ── Depth-prepass participation ──

TEST(DepthCoverageTest, TransparentGroupIsExcluded) {
    constexpr std::uint32_t transparent = 2u << 8; // BlendMode::Transparent
    EXPECT_FALSE(GroupWritesPrepassDepth(true, transparent, DepthCoverage::EngineDepthOnly));
    EXPECT_FALSE(GroupWritesPrepassDepth(true, transparent, DepthCoverage::Custom));
}

TEST(DepthCoverageTest, ExcludeRowIsExcluded) {
    EXPECT_FALSE(GroupWritesPrepassDepth(true, 0u, DepthCoverage::Exclude));
}

TEST(DepthCoverageTest, IncludedPoliciesWritePrepassDepth) {
    EXPECT_TRUE(GroupWritesPrepassDepth(true, 0u, DepthCoverage::EngineDepthOnly));
    EXPECT_TRUE(GroupWritesPrepassDepth(true, 0u, DepthCoverage::ReuseColorFragment));
    EXPECT_TRUE(GroupWritesPrepassDepth(true, 0u, DepthCoverage::Custom));
}

TEST(DepthCoverageTest, TechniqueOptOutWins) {
    EXPECT_FALSE(GroupWritesPrepassDepth(false, 0u, DepthCoverage::Custom));
}

TEST(DepthCoverageTest, RowLessTechniqueDefaultsToEngineDepthOnly) {
    TestTechnique tech;
    EXPECT_EQ(tech.GetDepthCoverage(0), DepthCoverage::EngineDepthOnly);
    // Only slot 0 exists without rows; any other slot is excluded.
    EXPECT_EQ(tech.GetDepthCoverage(1), DepthCoverage::Exclude);
    EXPECT_EQ(tech.GetVariantDepthShader(0), VulkanEngine::ShaderSystem::kInvalidShaderId);
}

TEST(DepthCoverageTest, RowWithoutPolicyDefaultsToExclude) {
    TestTechnique tech;
    const std::array<VariantShaderRow, 2> rows{{
        {.vert = 7u, .frag = 8u},
        {.vert = 9u, .frag = 10u},
    }};
    tech.SetVariantShaderRows(rows);
    EXPECT_EQ(tech.GetDepthCoverage(0), DepthCoverage::Exclude);
    EXPECT_EQ(tech.GetDepthCoverage(1), DepthCoverage::Exclude);
    EXPECT_EQ(tech.GetVariantDepthShader(0), VulkanEngine::ShaderSystem::kInvalidShaderId);
}

TEST(DepthCoverageTest, RowPolicyAndDepthShaderAreReported) {
    TestTechnique tech;
    const std::array<VariantShaderRow, 3> rows{{
        {.vert = 7u, .frag = 8u, .depth_frag = VulkanEngine::ShaderSystem::kInvalidShaderId,
         .depth_coverage = DepthCoverage::EngineDepthOnly},
        {.vert = 9u, .frag = 10u, .depth_frag = 11u, .depth_coverage = DepthCoverage::Custom},
        {.vert = 12u, .frag = 13u, .depth_frag = VulkanEngine::ShaderSystem::kInvalidShaderId,
         .depth_coverage = DepthCoverage::ReuseColorFragment},
    }};
    tech.SetVariantShaderRows(rows);
    EXPECT_EQ(tech.GetDepthCoverage(0), DepthCoverage::EngineDepthOnly);
    EXPECT_EQ(tech.GetDepthCoverage(1), DepthCoverage::Custom);
    EXPECT_EQ(tech.GetDepthCoverage(2), DepthCoverage::ReuseColorFragment);
    EXPECT_EQ(tech.GetVariantDepthShader(1), 11u);
    EXPECT_EQ(tech.GetVariantDepthShader(2), VulkanEngine::ShaderSystem::kInvalidShaderId);
    EXPECT_EQ(tech.GetDepthCoverage(3), DepthCoverage::Exclude);
    EXPECT_EQ(tech.GetVariantDepthShader(3), VulkanEngine::ShaderSystem::kInvalidShaderId);
}

// ── Material registration cross-check ──

TEST(DepthCoverageTest, UnlitDefaultsToReuseColorFragment) {
    VulkanEngine::TechniqueManager::UnlitTextureTechnique tech;
    EXPECT_EQ(tech.GetDepthCoverage(0),
              VulkanEngine::TechniqueManager::DepthCoverage::ReuseColorFragment);
}

TEST(DepthCoverageTest, CutoutMaterialUnderExcludeIsAccepted) {
    // Exclude is always safe: the main pass renders the material and only the
    // prepass skips it. A discarding payload must not be rejected for it.
    TechniqueManager mgr;
    const auto id = mgr.Register(std::make_unique<MaskTechnique>(DepthCoverage::Exclude));
    (void)id;
    MaterialRegistry materials;
    materials.SetTechniqueManager(&mgr);
    materials.Initialize(nullptr);

    VulkanEngine::TechniqueManager::UnlitPerMaterialData data{};
    data.albedo_texture = 1;
    const auto handle = materials.Register<MaskTechnique>(
        MaterialDesc{.blend = BlendMode::Cutout}, data);
    EXPECT_TRUE(handle.Valid());

    materials.Shutdown();
    mgr.Shutdown();
}

TEST(DepthCoverageTest, DiscardingMaterialUnderEngineDepthOnlyIsRejected) {
    // The mask payload always discards; EngineDepthOnly's empty prepass shader
    // cannot reproduce it, so registration must refuse the pair.
    TechniqueManager mgr;
    const auto id = mgr.Register(std::make_unique<MaskTechnique>(DepthCoverage::EngineDepthOnly));
    (void)id;
    MaterialRegistry materials;
    materials.SetTechniqueManager(&mgr);
    materials.Initialize(nullptr);

    VulkanEngine::TechniqueManager::UnlitPerMaterialData data{};
    data.albedo_texture = 1;
    const auto handle = materials.Register<MaskTechnique>(
        MaterialDesc{.blend = BlendMode::Cutout}, data);
    EXPECT_FALSE(handle.Valid());

    materials.Shutdown();
    mgr.Shutdown();
}

}  // namespace
