#include <gtest/gtest.h>

import std;
import std.compat;

import FileLoader.Types;
import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.FontResource;
import VulkanEngine.Text.Atlas;
import VulkanEngine.Text.Font;
import VulkanEngine.Text.Msdf;

namespace {

using VulkanEngine::FontResource;
using VulkanEngine::ResourceHandle;
using VulkanEngine::ResourceManager;
using VulkanEngine::Text::AtlasConfig;
using VulkanEngine::Text::AtlasPageFormat;
using VulkanEngine::Text::BytesPerTexel;
using VulkanEngine::Text::FontFace;
using VulkanEngine::Text::GlyphMetrics;
using VulkanEngine::Text::GlyphSlot;
using VulkanEngine::Text::HarfBuzzOutlineAdapter;
using VulkanEngine::Text::MsdfConfig;
using VulkanEngine::Text::MsdfGenerator;
using VulkanEngine::Text::MsdfGlyph;
using VulkanEngine::Text::OutlineEdgeKind;

[[nodiscard]] std::filesystem::path TestFontPath() {
    return std::filesystem::path{VKENGINE_TEST_FONT_DIR} / "Lato-Regular.ttf";
}

// The reconstruction rule for a multi-channel field: the median of the three
// channels is the signed distance, and 0.5 is the shape boundary.
[[nodiscard]] float MedianRgb(const std::vector<std::uint8_t>& pixels, std::size_t texel) {
    const std::size_t base = texel * MsdfGenerator::kChannels;
    const float r = static_cast<float>(pixels[base]) / 255.0f;
    const float g = static_cast<float>(pixels[base + 1]) / 255.0f;
    const float b = static_cast<float>(pixels[base + 2]) / 255.0f;
    return std::max(std::min(r, g), std::min(std::max(r, g), b));
}

// FNV-1a over the generated RGBA bytes. The value is pinned per msdfgen build,
// so it is only ever used as a determinism check, never as a definition of
// correctness.
[[nodiscard]] constexpr std::uint64_t HashField(const MsdfGlyph& glyph) {
    std::uint64_t hash = 0xCBF29CE484222325ULL;
    for (const std::uint8_t byte : glyph.pixels) {
        hash ^= byte;
        hash *= 0x100000001B3ULL;
    }
    return hash;
}

class MsdfTest : public ::testing::Test {
protected:
    void SetUp() override {
        handle_ = manager_.LoadFromFile<FontResource>(TestFontPath(),
                                                       ResourceManager::LoadSpeed::Instant);
        ASSERT_TRUE(handle_.IsValid());
        FontResource* resource = handle_.Get();
        ASSERT_NE(resource, nullptr);
        ASSERT_TRUE(resource->IsLoaded());
        face_ = FontFace::Create(*resource);
        ASSERT_NE(face_, nullptr);
        // A second face over the same bytes is a *different* face identity, which
        // is what the cache key must distinguish.
        second_face_ = FontFace::Create(*resource);
        ASSERT_NE(second_face_, nullptr);

        AtlasConfig config{};
        config.page_width = 512;
        config.page_height = 512;
        config.padding = 1;
        config.max_pages = 4;
        generator_ = std::make_unique<MsdfGenerator>(config, 64);
    }

    [[nodiscard]] std::uint32_t Glyph(char character) const {
        return face_->GlyphForCodepoint(static_cast<std::uint32_t>(character));
    }

    ResourceManager manager_;
    ResourceHandle<FontResource> handle_;
    std::shared_ptr<FontFace> face_;
    std::shared_ptr<FontFace> second_face_;
    std::unique_ptr<MsdfGenerator> generator_;
};

// A capital with only straight sides is drawn as line_to callbacks: one contour,
// every edge linear. Any mis-mapped callback (a quadratic or cubic where a line
// belongs) would show up here as a different edge kind.
TEST_F(MsdfTest, OutlineAdapterMapsACapitalToLineEdges) {
    HarfBuzzOutlineAdapter adapter;
    const std::uint32_t capital = Glyph('H');
    ASSERT_NE(capital, FontFace::MissingGlyph());

    const auto outline = adapter.Describe(*face_, capital);
    ASSERT_TRUE(outline.has_value());
    ASSERT_EQ(outline->contours.size(), 1u);
    EXPECT_TRUE(outline->contours.front().closed);
    EXPECT_GT(outline->EdgeCount(), 0u);
    EXPECT_TRUE(std::ranges::all_of(outline->contours.front().edges,
                                    [](OutlineEdgeKind kind) {
                                        return kind == OutlineEdgeKind::Line;
                                    }));
}

// A round glyph carries curves and a counter, so it must produce two contours
// (outer and hole) and at least one non-line edge through the quadratic/cubic
// callbacks.
TEST_F(MsdfTest, OutlineAdapterKeepsCurvesAndTheCounter) {
    HarfBuzzOutlineAdapter adapter;
    const std::uint32_t round = Glyph('O');
    ASSERT_NE(round, FontFace::MissingGlyph());

    const auto outline = adapter.Describe(*face_, round);
    ASSERT_TRUE(outline.has_value());
    ASSERT_EQ(outline->contours.size(), 2u);
    for (const auto& contour : outline->contours) {
        EXPECT_TRUE(contour.closed);
        EXPECT_FALSE(contour.edges.empty());
    }
    EXPECT_TRUE(std::ranges::any_of(outline->contours.front().edges,
                                    [](OutlineEdgeKind kind) {
                                        return kind != OutlineEdgeKind::Line;
                                    }));
}

// The generated field is a signed field with the shape's interior above 0.5 and
// its exterior below, and the interior sits in the middle of the padded box.
TEST_F(MsdfTest, GeneratedFieldHasBothSignsAndIsCentred) {
    const std::uint32_t capital = Glyph('H');
    const MsdfConfig config{.field_pixel_size = 32.0f, .range = 4.0};
    const auto glyph = generator_->Get(*face_, capital, config);
    ASSERT_NE(glyph, nullptr);
    ASSERT_FALSE(glyph->Empty());
    EXPECT_EQ(glyph->pixels.size(),
              static_cast<std::size_t>(glyph->width) * glyph->height * MsdfGenerator::kChannels);

    // The field is the ink box plus the range gutter on every side.
    const GlyphMetrics metrics = face_->GlyphAtSize(capital, config.field_pixel_size);
    const auto padding = static_cast<std::uint32_t>(std::ceil(config.range));
    EXPECT_EQ(glyph->width, static_cast<std::uint32_t>(std::ceil(metrics.width)) + 2U * padding);
    EXPECT_EQ(glyph->height, static_cast<std::uint32_t>(std::ceil(metrics.height)) + 2U * padding);
    EXPECT_FLOAT_EQ(glyph->left, metrics.bearing_x - static_cast<float>(padding));
    EXPECT_FLOAT_EQ(glyph->top, -metrics.bearing_y - static_cast<float>(padding));

    std::uint32_t interior = 0;
    std::uint32_t exterior = 0;
    double centroid_x = 0.0;
    double centroid_y = 0.0;
    for (std::uint32_t y = 0; y < glyph->height; ++y) {
        for (std::uint32_t x = 0; x < glyph->width; ++x) {
            const std::size_t texel = static_cast<std::size_t>(y) * glyph->width + x;
            if (MedianRgb(glyph->pixels, texel) > 0.5f) {
                ++interior;
                centroid_x += static_cast<double>(x);
                centroid_y += static_cast<double>(y);
            } else {
                ++exterior;
            }
        }
    }
    EXPECT_GT(interior, 0u) << "the generated field has no interior";
    EXPECT_GT(exterior, 0u) << "the generated field has no exterior";

    centroid_x /= static_cast<double>(interior);
    centroid_y /= static_cast<double>(interior);
    EXPECT_NEAR(centroid_x, static_cast<double>(glyph->width) * 0.5, 2.0)
        << "interior is not centred horizontally";
    EXPECT_NEAR(centroid_y, static_cast<double>(glyph->height) * 0.5, 2.0)
        << "interior is not centred vertically";

    // The alpha channel is the true single-channel distance and must carry the
    // same shape boundary, since the world shader consumes it.
    std::uint32_t alpha_inside = 0;
    for (std::uint32_t y = 0; y < glyph->height; ++y) {
        for (std::uint32_t x = 0; x < glyph->width; ++x) {
            const std::size_t base =
                (static_cast<std::size_t>(y) * glyph->width + x) * MsdfGenerator::kChannels;
            alpha_inside += glyph->pixels[base + 3] > 127 ? 1U : 0U;
        }
    }
    // The true field and the reconstructed one describe the same glyph; the
    // medians differ near corners, so only the order of magnitude is asserted.
    EXPECT_GT(alpha_inside, interior / 2);
    EXPECT_LT(alpha_inside, interior * 2);
}

// msdfgen-version-pinned: this hash is over the RGBA bytes of 'H' at a 32 px
// field with a 4 texel range, and it changes when msdfgen's generator or its
// outline handling changes -- which is exactly what an msdfgen bump can do. It
// must be updated in the same commit as such a bump, the way the GPU golden
// hashes and the FreeType-pinned glyph hash are. Current pin: msdfgen 1.12.1.
constexpr std::uint64_t kCapitalFieldHash32 = 0x7D9068DBB13105A6ULL;

TEST_F(MsdfTest, PinsTheGeneratedFieldBytes) {
    const std::uint32_t capital = Glyph('H');
    const auto glyph = generator_->Get(*face_, capital, MsdfConfig{32.0f, 4.0});
    ASSERT_NE(glyph, nullptr);
    EXPECT_EQ(HashField(*glyph), kCapitalFieldHash32);

    // The pin is a property of this exact request; a different field size is a
    // different set of bytes.
    const auto smaller = generator_->Get(*face_, capital, MsdfConfig{16.0f, 4.0});
    ASSERT_NE(smaller, nullptr);
    EXPECT_NE(HashField(*smaller), kCapitalFieldHash32);
}

// The cache is keyed on the exact request: the same request returns the same
// object, and any field that changes the bytes is a miss.
TEST_F(MsdfTest, CacheReturnsTheSameFieldAndMissesOnADifferentKey) {
    const std::uint32_t capital = Glyph('H');
    const MsdfConfig config{32.0f, 4.0};

    const auto first = generator_->Get(*face_, capital, config);
    const auto again = generator_->Get(*face_, capital, config);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first.get(), again.get());
    EXPECT_EQ(generator_->HitCount(), 1u);

    const auto bigger = generator_->Get(*face_, capital, MsdfConfig{48.0f, 4.0});
    ASSERT_NE(bigger, nullptr);
    EXPECT_NE(first.get(), bigger.get());

    const auto wider_range = generator_->Get(*face_, capital, MsdfConfig{32.0f, 8.0});
    ASSERT_NE(wider_range, nullptr);
    EXPECT_NE(first.get(), wider_range.get());

    // A different face identity never shares an entry, even over the same bytes.
    const auto other_face = generator_->Get(*second_face_, capital, config);
    ASSERT_NE(other_face, nullptr);
    EXPECT_NE(first.get(), other_face.get());

    // A different glyph is a different field.
    const auto other_glyph = generator_->Get(*face_, Glyph('I'), config);
    ASSERT_NE(other_glyph, nullptr);
    EXPECT_NE(first.get(), other_glyph.get());
}

// A generated field lands in the Rgba8 atlas page the world shader samples: the
// slot is padded by the atlas gutter, the entry hands the uploader four channels
// per texel, and the page format reports Rgba8.
TEST_F(MsdfTest, AtlasSlotUsesTheChosenPageFormat) {
    EXPECT_EQ(MsdfGenerator::kPageFormat, AtlasPageFormat::Rgba8);
    EXPECT_EQ(BytesPerTexel(MsdfGenerator::kPageFormat), 4u);
    EXPECT_EQ(BytesPerTexel(AtlasPageFormat::A8), 1u);

    const std::uint32_t capital = Glyph('H');
    const auto slot = generator_->Generate(*face_, capital, MsdfConfig{32.0f, 4.0});
    ASSERT_TRUE(slot.has_value());
    const auto glyph = generator_->Get(*face_, capital, MsdfConfig{32.0f, 4.0});
    ASSERT_NE(glyph, nullptr);

    const AtlasConfig& atlas = generator_->Atlas().Config();
    EXPECT_EQ(slot->rect.width, glyph->width + 2U * atlas.padding);
    EXPECT_EQ(slot->rect.height, glyph->height + 2U * atlas.padding);

    const auto entries = generator_->Atlas().Entries();
    ASSERT_EQ(entries.size(), 1u);
    const auto bitmap = generator_->BitmapForAtlasKey(entries.front().first);
    ASSERT_NE(bitmap, nullptr);
    EXPECT_EQ(bitmap->width, glyph->width);
    EXPECT_EQ(bitmap->height, glyph->height);
    EXPECT_EQ(bitmap->bytes.size(), glyph->pixels.size());
    EXPECT_EQ(bitmap->bytes, glyph->pixels);
    EXPECT_EQ(generator_->BitmapForAtlasKey(0xDEADBEEFULL), nullptr);
}

// An inkless glyph still produces a field object (so a caller can ask "what is
// this glyph's advance?"), but it has no pixels and reserves no atlas slot.
TEST_F(MsdfTest, InklessGlyphGeneratesNoPixelsAndNoSlot) {
    const std::uint32_t space = Glyph(' ');
    ASSERT_NE(space, FontFace::MissingGlyph());

    const auto slot = generator_->Generate(*face_, space, MsdfConfig{32.0f, 4.0});
    EXPECT_FALSE(slot.has_value());
    const auto glyph = generator_->Get(*face_, space, MsdfConfig{32.0f, 4.0});
    ASSERT_NE(glyph, nullptr);
    EXPECT_TRUE(glyph->Empty());
    EXPECT_TRUE(glyph->pixels.empty());
    EXPECT_GT(glyph->advance_x, 0.0f);
    EXPECT_EQ(generator_->Atlas().GlyphCount(), 0u);
}

// A zero or negative field size is "do not generate", matching QuantizeFieldSize
// and the rasterizer's rule.
TEST_F(MsdfTest, ZeroFieldSizeGeneratesNothing) {
    const std::uint32_t capital = Glyph('H');
    EXPECT_EQ(generator_->Get(*face_, capital, MsdfConfig{0.0f, 4.0}), nullptr);
    EXPECT_EQ(generator_->Get(*face_, capital, MsdfConfig{-8.0f, 4.0}), nullptr);
    EXPECT_EQ(generator_->Size(), 0u);
}

} // namespace
