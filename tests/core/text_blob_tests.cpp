#include <gtest/gtest.h>

import std;
import std.compat;

import FileLoader.Types;
import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.FontResource;
import VulkanEngine.Text.Blob;
import VulkanEngine.Text.Font;

namespace {

using VulkanEngine::FontResource;
using VulkanEngine::ResourceHandle;
using VulkanEngine::ResourceManager;
using VulkanEngine::Text::BlobRangeAllocator;
using VulkanEngine::Text::FontFace;
using VulkanEngine::Text::GlyphBlob;
using VulkanEngine::Text::GlyphBlobEncoder;

[[nodiscard]] std::filesystem::path TestFontPath() {
    return std::filesystem::path{VKENGINE_TEST_FONT_DIR} / "Lato-Regular.ttf";
}

// Device-free: the encoder is HarfBuzz's hb-gpu blob encoder over the shared
// face and the range allocator is pure integer state. Neither touches a device,
// which is what lets the packing rules and the size-independence of the cache be
// pinned here rather than in the GPU suite.
class TextBlobTest : public ::testing::Test {
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
        // A second face over the same bytes is a different identity, which is
        // what the cache key must distinguish.
        second_face_ = FontFace::Create(*resource);
        ASSERT_NE(second_face_, nullptr);
    }

    [[nodiscard]] std::uint32_t Glyph(char character) const {
        return face_->GlyphForCodepoint(static_cast<std::uint32_t>(character));
    }

    ResourceManager manager_;
    ResourceHandle<FontResource> handle_;
    std::shared_ptr<FontFace> face_;
    std::shared_ptr<FontFace> second_face_;
};

// The measurement that defines the blob's addressable unit: an encoded blob is
// always a whole number of 8-byte units, and is NOT always a whole number of
// 16. That is what rules out the 16-byte `int4` stride the upstream HLSL header
// comment suggests, and what the GPU readback test then checks end to end.
TEST_F(TextBlobTest, EncodedBlobsAreWholeEightByteUnits) {
    GlyphBlobEncoder encoder;
    std::size_t multiple_of_eight = 0;
    std::size_t not_multiple_of_sixteen = 0;
    for (const char character : {'H', 'o', '.', '8', 'W'}) {
        const auto blob = encoder.Get(*face_, Glyph(character));
        ASSERT_NE(blob, nullptr) << "no blob for '" << character << "'";
        ASSERT_FALSE(blob->Empty()) << "no ink for '" << character << "'";
        EXPECT_EQ(blob->bytes.size() % 8U, 0U)
            << "the blob is not a whole number of 8-byte units";
        EXPECT_EQ(blob->UnitCount(), blob->bytes.size() / 8U);
        ++multiple_of_eight;
        if (blob->bytes.size() % 16U != 0U) {
            ++not_multiple_of_sixteen;
        }
    }
    EXPECT_EQ(multiple_of_eight, 5u);
    // The 16-byte-stride mistake is only observable if at least one measured
    // glyph is 8 mod 16; otherwise "always a multiple of 16" would also pass.
    EXPECT_GT(not_multiple_of_sixteen, 0u);
}

// Encoding is deterministic: the same glyph through the same face produces the
// same bytes and the same extents, so a caller can cache or compare them.
TEST_F(TextBlobTest, EncodingIsDeterministic) {
    GlyphBlobEncoder encoder;
    const std::uint32_t capital = Glyph('H');

    const auto first = encoder.Get(*face_, capital);
    ASSERT_NE(first, nullptr);
    ASSERT_FALSE(first->Empty());
    const std::vector<std::byte> bytes = first->bytes;
    const std::int32_t x_bearing = first->x_bearing;
    const std::int32_t y_bearing = first->y_bearing;
    const std::int32_t width = first->width;
    const std::int32_t height = first->height;

    encoder.Clear();
    const auto second = encoder.Get(*face_, capital);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->bytes, bytes);
    EXPECT_EQ(second->x_bearing, x_bearing);
    EXPECT_EQ(second->y_bearing, y_bearing);
    EXPECT_EQ(second->width, width);
    EXPECT_EQ(second->height, height);
    // A capital's box is a real box: positive width, positive top bearing and a
    // negative height (the y-up box extends downward from its top bearing).
    EXPECT_GT(width, 0);
    EXPECT_GT(y_bearing, 0);
    EXPECT_LT(height, 0);
}

// The whole reason the Slug path exists: the blob is in font design units, so
// two requests at different pixel sizes are one encode and one cache entry.
TEST_F(TextBlobTest, CacheIsSizeIndependentAndEncodesOnce) {
    GlyphBlobEncoder encoder;
    const std::uint32_t capital = Glyph('H');

    const auto small = encoder.Get(*face_, capital, 16.0f);
    const auto large = encoder.Get(*face_, capital, 64.0f);
    ASSERT_NE(small, nullptr);
    ASSERT_NE(large, nullptr);
    EXPECT_EQ(small.get(), large.get()) << "two sizes produced two blobs";
    EXPECT_EQ(encoder.EncodeCount(), 1u) << "the second size re-encoded the glyph";
    EXPECT_EQ(encoder.HitCount(), 1u);
    EXPECT_EQ(encoder.Size(), 1u);

    // A different glyph and a different face identity are real misses, so the
    // hit above is not just "the cache always says yes".
    static_cast<void>(encoder.Get(*face_, Glyph('o'), 16.0f));
    static_cast<void>(encoder.Get(*second_face_, capital, 16.0f));
    EXPECT_EQ(encoder.EncodeCount(), 3u);
    EXPECT_EQ(encoder.Size(), 3u);

    // Caching disabled still returns a usable blob.
    GlyphBlobEncoder uncached(0);
    const auto one = uncached.Get(*face_, capital);
    const auto two = uncached.Get(*face_, capital);
    ASSERT_NE(one, nullptr);
    ASSERT_NE(two, nullptr);
    EXPECT_EQ(uncached.EncodeCount(), 2u);
    EXPECT_EQ(uncached.Size(), 0u);
}

// A blob a caller already holds survives eviction, exactly like the rasterizer's
// and the MSDF store's shared_ptr results.
TEST_F(TextBlobTest, EvictedBlobsStayAlive) {
    GlyphBlobEncoder encoder(2);
    const auto capital = encoder.Get(*face_, Glyph('H'));
    ASSERT_NE(capital, nullptr);
    const std::vector<std::byte> bytes = capital->bytes;

    static_cast<void>(encoder.Get(*face_, Glyph('o')));
    static_cast<void>(encoder.Get(*face_, Glyph('.')));
    EXPECT_EQ(encoder.Size(), 2u);

    // Whatever eviction did, the held blob is unchanged and readable.
    EXPECT_EQ(capital->bytes, bytes);
    EXPECT_FALSE(capital->Empty());
}

// A space has no ink: the encoder still hands back a blob object (so a caller
// can ask for its extents and advance the pen), but its bytes are empty and its
// unit count is zero, which is what stops the buffer allocating it a range.
TEST_F(TextBlobTest, SpaceEncodesToZeroLength) {
    GlyphBlobEncoder encoder;
    const std::uint32_t space = Glyph(' ');
    ASSERT_NE(space, FontFace::MissingGlyph());

    const auto blob = encoder.Get(*face_, space);
    ASSERT_NE(blob, nullptr);
    EXPECT_TRUE(blob->Empty());
    EXPECT_TRUE(blob->bytes.empty());
    EXPECT_EQ(blob->UnitCount(), 0u);
}

// The allocator packs blobs at 8-byte-aligned offsets that never overlap, and
// its capacity is exactly the high-water mark.
TEST_F(TextBlobTest, AllocatorPacksWithoutOverlapAtEightByteAlignment) {
    BlobRangeAllocator allocator;
    EXPECT_FALSE(allocator.Allocate(0).has_value()) << "a zero-byte request must take no range";

    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    // Deliberately awkward sizes (not multiples of 8) so the rounding is
    // exercised: what must be aligned is the offset the shader indexes, not the
    // requested length.
    for (const std::uint64_t size : {1528ULL, 2928ULL, 3ULL, 1880ULL, 17ULL, 4680ULL}) {
        const auto offset = allocator.Allocate(size);
        ASSERT_TRUE(offset.has_value());
        EXPECT_EQ(*offset % BlobRangeAllocator::kAlignment, 0U);
        ranges.emplace_back(*offset, size);
    }
    EXPECT_EQ(allocator.LiveCount(), ranges.size());

    std::ranges::sort(ranges);
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const std::uint64_t rounded =
            ((ranges[i].second + 7U) / 8U) * 8U;
        if (i + 1 < ranges.size()) {
            EXPECT_LE(ranges[i].first + rounded, ranges[i + 1].first)
                << "ranges " << i << " and " << (i + 1) << " overlap";
        }
    }
    EXPECT_EQ(allocator.Capacity(), ranges.back().first +
                                        ((ranges.back().second + 7U) / 8U) * 8U);
    EXPECT_EQ(allocator.UsedBytes(), allocator.Capacity());
}

// A freed range is reusable in place rather than stranded: freeing a middle
// range and asking for one of the same size must hand the freed offset back, and
// the reused range must not overlap a live one.
TEST_F(TextBlobTest, AllocatorReusesFreedRanges) {
    BlobRangeAllocator allocator;
    const auto first = allocator.Allocate(64);
    const auto middle = allocator.Allocate(128);
    const auto last = allocator.Allocate(64);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(middle.has_value());
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(*first, 0u);
    EXPECT_EQ(*middle, 64u);
    EXPECT_EQ(*last, 192u);
    EXPECT_EQ(allocator.Capacity(), 256u);
    const std::uint64_t capacity_before = allocator.Capacity();

    allocator.Free(*middle);
    EXPECT_EQ(allocator.UsedBytes(), 128u);
    EXPECT_EQ(allocator.LiveCount(), 2u);
    EXPECT_EQ(allocator.FreeBlockCount(), 1u);

    const auto reused = allocator.Allocate(100);
    ASSERT_TRUE(reused.has_value());
    EXPECT_EQ(*reused, *middle) << "the freed range was not reused";
    EXPECT_EQ(allocator.Capacity(), capacity_before) << "reuse grew the buffer";
    // The 128-byte freed range held a 104-byte (100 rounded up) allocation, so
    // the 24-byte tail is still free and waiting for a small blob.
    EXPECT_EQ(allocator.FreeBlockCount(), 1u);

    // The reused range cannot overlap either live neighbour.
    EXPECT_GE(*reused, *first + 64U);
    EXPECT_LE(*reused + 104U, *last);

    // Freeing everything and allocating again restarts from offset 0, because
    // the freed blocks coalesce back into one range.
    allocator.Free(*reused);
    allocator.Free(*first);
    allocator.Free(*last);
    EXPECT_EQ(allocator.UsedBytes(), 0u);
    EXPECT_EQ(allocator.FreeBlockCount(), 1u);
    EXPECT_EQ(allocator.Allocate(200), 0u);
}

} // namespace
